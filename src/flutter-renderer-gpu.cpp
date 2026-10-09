#include "flutter-renderer-internal.hpp"
#include <obs-module.h>
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES2/gl2.h>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
void check_hr(HRESULT result, const char *operation)
{
	if (FAILED(result)) {
		blog(LOG_ERROR, "[FlutterSource GPU] %s failed (0x%08lx)", operation, result);
		throw std::runtime_error(operation);
	}
}

/* Explicit loading keeps the software source usable without ANGLE installed.
 * A private directory also avoids mixing our EGL/GLES pair with obs-browser's. */
class Angle {
public:
	~Angle()
	{
		if (egl)
			FreeLibrary(egl);
		if (gles)
			FreeLibrary(gles);
	}
	void load()
	{
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				   reinterpret_cast<LPCWSTR>(&flutter_gpu_renderer_create), &module);
		std::wstring path(32768, L'\0');
		const DWORD length = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
		if (!length || length >= path.size())
			throw std::runtime_error("Cannot resolve the plugin directory");
		path.resize(length);
		path.resize(path.find_last_of(L"\\/") + 1);
		path += L"flutter_obs_source\\angle\\";
		const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
		gles = LoadLibraryExW((path + L"libGLESv2.dll").c_str(), nullptr, flags);
		if (gles)
			egl = LoadLibraryExW((path + L"libEGL.dll").c_str(), nullptr, flags);
		if (!gles || !egl) {
			blog(LOG_ERROR, "[FlutterSource GPU] Cannot load ANGLE from %ls (Windows error %lu)",
			     path.c_str(), GetLastError());
			throw std::runtime_error("Install the packaged ANGLE runtime to use the GPU source");
		}
#define LOAD_EGL(name)                                                               \
	name = reinterpret_cast<decltype(name)>(::GetProcAddress(egl, "egl" #name)); \
	if (!name)                                                                   \
	throw std::runtime_error("Missing egl" #name)
		LOAD_EGL(GetProcAddress);
		LOAD_EGL(Initialize);
		LOAD_EGL(Terminate);
		LOAD_EGL(ChooseConfig);
		LOAD_EGL(CreateContext);
		LOAD_EGL(DestroyContext);
		LOAD_EGL(CreatePbufferSurface);
		LOAD_EGL(CreatePbufferFromClientBuffer);
		LOAD_EGL(DestroySurface);
		LOAD_EGL(MakeCurrent);
		LOAD_EGL(GetError);
#undef LOAD_EGL
#define LOAD_EXT(name)                                                        \
	name = reinterpret_cast<decltype(name)>(GetProcAddress("egl" #name)); \
	if (!name)                                                            \
	throw std::runtime_error("Missing egl" #name)
		LOAD_EXT(CreateDeviceANGLE);
		LOAD_EXT(ReleaseDeviceANGLE);
		LOAD_EXT(GetPlatformDisplayEXT);
#undef LOAD_EXT
		Finish = reinterpret_cast<decltype(Finish)>(GetProcAddress("glFinish"));
		if (!Finish)
			throw std::runtime_error("Missing glFinish");
	}
	void check(bool success, const char *operation)
	{
		if (!success) {
			blog(LOG_ERROR, "[FlutterSource GPU] %s failed (EGL error 0x%x)", operation, GetError());
			throw std::runtime_error(operation);
		}
	}

	decltype(&eglGetProcAddress) GetProcAddress = nullptr;
	decltype(&eglInitialize) Initialize = nullptr;
	decltype(&eglTerminate) Terminate = nullptr;
	decltype(&eglChooseConfig) ChooseConfig = nullptr;
	decltype(&eglCreateContext) CreateContext = nullptr;
	decltype(&eglDestroyContext) DestroyContext = nullptr;
	decltype(&eglCreatePbufferSurface) CreatePbufferSurface = nullptr;
	decltype(&eglCreatePbufferFromClientBuffer) CreatePbufferFromClientBuffer = nullptr;
	decltype(&eglDestroySurface) DestroySurface = nullptr;
	decltype(&eglMakeCurrent) MakeCurrent = nullptr;
	decltype(&eglGetError) GetError = nullptr;
	PFNEGLCREATEDEVICEANGLEPROC CreateDeviceANGLE = nullptr;
	PFNEGLRELEASEDEVICEANGLEPROC ReleaseDeviceANGLE = nullptr;
	PFNEGLGETPLATFORMDISPLAYEXTPROC GetPlatformDisplayEXT = nullptr;
	decltype(&glFinish) Finish = nullptr;

private:
	HMODULE egl = nullptr;
	HMODULE gles = nullptr;
};

enum class BufferState { free, writing, ready, reading };
struct GpuBuffer {
	ComPtr<ID3D11Texture2D> texture;
	ComPtr<IDXGIKeyedMutex> keyed_mutex;
	uint32_t handle = GS_INVALID_HANDLE;
	uint32_t width = 0;
	uint32_t height = 0;
	uint64_t acquire_key = 0;
	BufferState state = BufferState::free;
};
struct ObsBuffer {
	std::shared_ptr<GpuBuffer> buffer;
	gs_texture_t *texture = nullptr;
};

class GpuRenderer final : public flutter_renderer {
public:
	using flutter_renderer::flutter_renderer;
	~GpuRenderer() override
	{
		/* Engine shutdown has joined raster/IO threads before this runs. */
		if (egl_display != EGL_NO_DISPLAY) {
			angle.MakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			if (surface != EGL_NO_SURFACE)
				angle.DestroySurface(egl_display, surface);
			if (resource_surface != EGL_NO_SURFACE)
				angle.DestroySurface(egl_display, resource_surface);
			if (render_context != EGL_NO_CONTEXT)
				angle.DestroyContext(egl_display, render_context);
			if (resource_context != EGL_NO_CONTEXT)
				angle.DestroyContext(egl_display, resource_context);
			angle.Terminate(egl_display);
		}
		if (egl_device != EGL_NO_DEVICE_EXT)
			angle.ReleaseDeviceANGLE(egl_device);
		obs_enter_graphics();
		for (auto &cached : obs_buffers)
			gs_texture_destroy(cached.texture);
		gs_texture_destroy(display);
		obs_leave_graphics();
	}

	void initialize()
	{
		ComPtr<IDXGIAdapter> adapter;
		obs_enter_graphics();
		const bool supported = gs_get_device_type() == GS_DEVICE_DIRECT3D_11;
		HRESULT result = E_FAIL;
		if (supported) {
			auto *obs_device = static_cast<ID3D11Device *>(gs_get_device_obj());
			ComPtr<IDXGIDevice> dxgi_device;
			result = obs_device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
			if (SUCCEEDED(result))
				result = dxgi_device->GetAdapter(&adapter);
		}
		obs_leave_graphics();
		if (!supported)
			throw std::runtime_error("The GPU source requires OBS Direct3D 11");
		check_hr(result, "Get OBS GPU adapter");
		const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
		check_hr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
					   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION, &device,
					   nullptr, &device_context),
			 "Create private D3D11 device");
		ComPtr<ID3D11Multithread> multithread;
		check_hr(device_context.As(&multithread), "Enable D3D11 multithread protection");
		multithread->SetMultithreadProtected(TRUE);
		angle.load();
		egl_device = angle.CreateDeviceANGLE(EGL_D3D11_DEVICE_ANGLE, device.Get(), nullptr);
		angle.check(egl_device != EGL_NO_DEVICE_EXT, "eglCreateDeviceANGLE");
		egl_display = angle.GetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, egl_device, nullptr);
		angle.check(egl_display != EGL_NO_DISPLAY, "eglGetPlatformDisplayEXT");
		angle.check(angle.Initialize(egl_display, nullptr, nullptr), "eglInitialize");
		const EGLint config_attributes[] = {EGL_SURFACE_TYPE,
						    EGL_PBUFFER_BIT,
						    EGL_RENDERABLE_TYPE,
						    EGL_OPENGL_ES2_BIT,
						    EGL_RED_SIZE,
						    8,
						    EGL_GREEN_SIZE,
						    8,
						    EGL_BLUE_SIZE,
						    8,
						    EGL_ALPHA_SIZE,
						    8,
						    EGL_DEPTH_SIZE,
						    0,
						    EGL_STENCIL_SIZE,
						    8,
						    EGL_NONE};
		EGLint count = 0;
		angle.check(angle.ChooseConfig(egl_display, config_attributes, &egl_config, 1, &count) && count == 1,
			    "eglChooseConfig");
		const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
		render_context = angle.CreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attributes);
		angle.check(render_context != EGL_NO_CONTEXT, "Create render context");
		resource_context = angle.CreateContext(egl_display, egl_config, render_context, context_attributes);
		angle.check(resource_context != EGL_NO_CONTEXT, "Create shared resource context");
		const EGLint resource_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
		resource_surface = angle.CreatePbufferSurface(egl_display, egl_config, resource_attributes);
		angle.check(resource_surface != EGL_NO_SURFACE, "Create resource surface");
		ensure_surface(width, height);
		blog(LOG_INFO, "[FlutterSource GPU] ANGLE/D3D11 initialized on the OBS adapter");
	}

	FlutterRendererConfig config() override
	{
		FlutterRendererConfig result = {};
		result.type = kOpenGL;
		result.open_gl.struct_size = sizeof(result.open_gl);
		result.open_gl.make_current = make_current;
		result.open_gl.clear_current = clear_current;
		result.open_gl.make_resource_current = make_resource_current;
		result.open_gl.fbo_with_frame_info_callback = framebuffer;
		result.open_gl.fbo_reset_after_present = true;
		result.open_gl.present = present;
		result.open_gl.gl_proc_resolver = resolve;
		return result;
	}

	uint32_t draw_flags() const override { return GS_FLIP_V; }

	gs_texture_t *texture() override
	{
		std::shared_ptr<GpuBuffer> next;
		const auto [requested_width, requested_height] = size();
		{
			std::lock_guard<std::mutex> lock(mutex);
			next = std::move(ready);
			if (next)
				next->state = BufferState::reading;
		}
		if (next) {
			auto &cached = obs_buffers[cache_index++ % obs_buffers.size()];
			/* Cache native imports across frames, including old generations until
			 * the graphics thread can release them safely. */
			for (auto &entry : obs_buffers) {
				if (entry.buffer == next) {
					consume(next, entry);
					next.reset();
					break;
				}
			}
			if (next) {
				gs_texture_destroy(cached.texture);
				cached.buffer = next;
				cached.texture = gs_texture_open_shared(next->handle);
				consume(next, cached);
			}
		}
		if (display && (gs_texture_get_width(display) != requested_width ||
				gs_texture_get_height(display) != requested_height)) {
			gs_texture_destroy(display);
			display = nullptr;
		}
		return display;
	}

private:
	void invalidate_ready() override
	{
		if (ready)
			ready->state = BufferState::free;
		ready.reset();
	}

	void consume(const std::shared_ptr<GpuBuffer> &buffer, ObsBuffer &cached)
	{
		bool released = false;
		if (cached.texture && gs_texture_acquire_sync(cached.texture, 1, 0) == 0) {
			if (!display || gs_texture_get_width(display) != buffer->width ||
			    gs_texture_get_height(display) != buffer->height) {
				gs_texture_destroy(display);
				display = gs_texture_create(buffer->width, buffer->height, GS_RGBA, 1, nullptr, 0);
			}
			if (display)
				gs_copy_texture(display, cached.texture);
			released = gs_texture_release_sync(cached.texture, 0) == 0;
		}
		std::lock_guard<std::mutex> lock(mutex);
		buffer->state = BufferState::free;
		if (released)
			buffer->acquire_key = 0;
		else if (!ready && buffer->width == width && buffer->height == height) {
			buffer->state = BufferState::ready;
			ready = buffer;
		}
	}

	ComPtr<ID3D11Texture2D> create_texture(uint32_t texture_width, uint32_t texture_height, bool shared)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = texture_width;
		desc.Height = texture_height;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = shared ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : 0;
		ComPtr<ID3D11Texture2D> texture;
		check_hr(device->CreateTexture2D(&desc, nullptr, &texture), "Create GPU frame texture");
		return texture;
	}

	void ensure_surface(uint32_t new_width, uint32_t new_height)
	{
		if (surface != EGL_NO_SURFACE && surface_width == new_width && surface_height == new_height)
			return;
		auto new_texture = create_texture(new_width, new_height, false);
		std::array<std::shared_ptr<GpuBuffer>, 3> new_buffers;
		for (auto &buffer : new_buffers) {
			buffer = std::make_shared<GpuBuffer>();
			buffer->width = new_width;
			buffer->height = new_height;
			buffer->texture = create_texture(new_width, new_height, true);
			check_hr(buffer->texture.As(&buffer->keyed_mutex), "Get shared texture mutex");
			ComPtr<IDXGIResource> resource;
			check_hr(buffer->texture.As(&resource), "Get shared texture resource");
			HANDLE handle = nullptr;
			check_hr(resource->GetSharedHandle(&handle), "Get shared texture handle");
			buffer->handle = uint32_t(reinterpret_cast<uintptr_t>(handle));
		}
		const EGLint attributes[] = {EGL_NONE};
		EGLSurface new_surface = angle.CreatePbufferFromClientBuffer(egl_display, EGL_D3D_TEXTURE_ANGLE,
									     new_texture.Get(), egl_config, attributes);
		angle.check(new_surface != EGL_NO_SURFACE, "Import D3D11 render target into EGL");
		/* Allocate everything before replacing the current generation, so an
		 * allocation failure leaves a usable surface for a later retry. */
		if (!angle.MakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
			angle.DestroySurface(egl_display, new_surface);
			angle.check(false, "Unbind old render surface");
		}
		if (surface != EGL_NO_SURFACE)
			angle.DestroySurface(egl_display, surface);
		surface = new_surface;
		render_target = new_texture;
		surface_width = new_width;
		surface_height = new_height;
		std::lock_guard<std::mutex> lock(mutex);
		buffers = std::move(new_buffers);
		ready.reset();
	}

	static GpuRenderer &self(void *data)
	{
		return *static_cast<GpuRenderer *>(static_cast<flutter_renderer *>(data));
	}
	static bool make_current(void *data)
	{
		auto &gpu = self(data);
		return gpu.angle.MakeCurrent(gpu.egl_display, gpu.surface, gpu.surface, gpu.render_context) == EGL_TRUE;
	}
	static bool clear_current(void *data)
	{
		auto &gpu = self(data);
		return gpu.angle.MakeCurrent(gpu.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ==
		       EGL_TRUE;
	}
	static bool make_resource_current(void *data)
	{
		auto &gpu = self(data);
		return gpu.angle.MakeCurrent(gpu.egl_display, gpu.resource_surface, gpu.resource_surface,
					     gpu.resource_context) == EGL_TRUE;
	}
	static void *resolve(void *data, const char *name)
	{
		return reinterpret_cast<void *>(self(data).angle.GetProcAddress(name));
	}
	static uint32_t framebuffer(void *data, const FlutterFrameInfo *info)
	{
		auto &gpu = self(data);
		try {
			gpu.ensure_surface(uint32_t(info->size.width), uint32_t(info->size.height));
			gpu.angle.check(make_current(data), "Bind resized render target");
			gpu.surface_failed = false;
		} catch (const std::exception &error) {
			gpu.surface_failed = true;
			blog(LOG_ERROR, "[FlutterSource GPU] Cannot resize render target: %s", error.what());
		}
		return 0; // The imported EGL pbuffer is the default framebuffer.
	}
	static bool present(void *data)
	{
		auto &gpu = self(data);
		if (gpu.surface_failed)
			return false;
		std::shared_ptr<GpuBuffer> buffer;
		uint64_t generation;
		{
			std::lock_guard<std::mutex> lock(gpu.mutex);
			generation = gpu.generation;
			if (gpu.surface_width != gpu.width || gpu.surface_height != gpu.height)
				return true;
			for (auto &candidate : gpu.buffers) {
				if (candidate->state == BufferState::free) {
					buffer = candidate;
					buffer->state = BufferState::writing;
					break;
				}
			}
		}
		if (!buffer)
			return true;
		if (buffer->keyed_mutex->AcquireSync(buffer->acquire_key, 0) != S_OK) {
			std::lock_guard<std::mutex> lock(gpu.mutex);
			buffer->state = BufferState::free;
			return true;
		}
		/* Finish EGL commands before accessing its native texture. Copies and
		 * keyed mutex ownership keep all pixels on the GPU; no readback occurs. */
		gpu.angle.Finish();
		if (!clear_current(data)) {
			buffer->keyed_mutex->ReleaseSync(buffer->acquire_key);
			std::lock_guard<std::mutex> lock(gpu.mutex);
			buffer->state = BufferState::free;
			return false;
		}
		gpu.device_context->CopyResource(buffer->texture.Get(), gpu.render_target.Get());
		gpu.device_context->Flush();
		const HRESULT result = buffer->keyed_mutex->ReleaseSync(1);
		const bool restored = make_current(data);
		std::lock_guard<std::mutex> lock(gpu.mutex);
		buffer->state = BufferState::free;
		buffer->acquire_key = 1;
		if (FAILED(result)) {
			blog(LOG_ERROR, "[FlutterSource GPU] Frame publication failed (0x%08lx)", result);
			return false;
		}
		if (generation == gpu.generation) {
			if (gpu.ready)
				gpu.ready->state = BufferState::free;
			buffer->state = BufferState::ready;
			gpu.ready = buffer;
		}
		return restored;
	}

	Angle angle;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> device_context;
	ComPtr<ID3D11Texture2D> render_target;
	EGLDeviceEXT egl_device = EGL_NO_DEVICE_EXT;
	EGLDisplay egl_display = EGL_NO_DISPLAY;
	EGLConfig egl_config = nullptr;
	EGLContext render_context = EGL_NO_CONTEXT;
	EGLContext resource_context = EGL_NO_CONTEXT;
	EGLSurface surface = EGL_NO_SURFACE;
	EGLSurface resource_surface = EGL_NO_SURFACE;
	uint32_t surface_width = 0;
	uint32_t surface_height = 0;
	bool surface_failed = false;
	std::array<std::shared_ptr<GpuBuffer>, 3> buffers;
	std::shared_ptr<GpuBuffer> ready;
	std::array<ObsBuffer, 3> obs_buffers;
	size_t cache_index = 0;
	gs_texture_t *display = nullptr;
};
} // namespace

flutter_renderer *flutter_gpu_renderer_create(uint32_t width, uint32_t height, void *owner)
{
	auto renderer = std::make_unique<GpuRenderer>(width, height, owner);
	renderer->initialize();
	return renderer.release();
}
