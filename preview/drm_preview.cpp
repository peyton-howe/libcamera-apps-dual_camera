/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * drm_preview.cpp - DRM-based preview window.
 */

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "core/options.hpp"

#include "preview.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <gbm.h>
#include "mesh.hpp"
#include <stdlib.h>
#include <fcntl.h>

Mesh *SSQuad1;
class DrmPreview : public Preview
{
public:
	DrmPreview(Options const *options);
	~DrmPreview();
	// Display the buffer. You get given the fd back in the BufferDoneCallback
	// once its available for re-use.
	virtual void Show(int fd, libcamera::Span<uint8_t> span, StreamInfo const &info) override;
	// Reset the preview window, clearing the current buffers and being ready to
	// show new ones.
	virtual void Reset() override;
	// Return the maximum image size allowed.
	virtual void MaxImageSize(unsigned int &w, unsigned int &h) const override
	{
		w = max_image_width_;
		h = max_image_height_;
	}

private:
	struct Buffer
	{
		Buffer() : fd(-1) {}
		int fd;
		size_t size;
		StreamInfo info;
		uint32_t bo_handle;
		unsigned int fb_handle;
		GLuint texture;
	};
	void makeBuffer(int fd, size_t size, StreamInfo const &info, Buffer &buffer);
	EGLDisplay egl_display_;
	EGLContext egl_context_;
	EGLSurface egl_surface_;
	void findCrtc();
	void findPlane();
	int drmfd_;
	int conId_;
	uint32_t crtcId_;
	int crtcIdx_;
	uint32_t planeId_;
	unsigned int out_fourcc_;
	unsigned int x_;
	unsigned int y_;
	unsigned int width_;
	unsigned int height_;
	unsigned int screen_width_;
	unsigned int screen_height_;
	std::map<int, Buffer> buffers_; // map the DMABUF's fd to the Buffer
	int last_fd_;
	unsigned int max_image_width_;
	unsigned int max_image_height_;
	bool first_time_;
};

#define ERRSTR strerror(errno)
struct gbm_surface *gbm_surface;
struct gbm_device *gbm_device;
struct gbm_bo *bo;
uint32_t handle;
uint32_t pitch;
uint32_t fb;
uint64_t modifier;

static GLint compile_shader(GLenum target, const char *source)
{
	GLuint s = glCreateShader(target);
	glShaderSource(s, 1, (const GLchar **)&source, NULL);
	glCompileShader(s);

	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);

	if (!ok)
	{
		GLchar *info;
		GLint size;

		glGetShaderiv(s, GL_INFO_LOG_LENGTH, &size);
		info = (GLchar *)malloc(size);

		glGetShaderInfoLog(s, size, NULL, info);
		throw std::runtime_error("failed to compile shader: " + std::string(info) + "\nsource:\n" +
								 std::string(source));
	}

	return s;
}

static GLint link_program(GLint vs, GLint fs)
{
	GLint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	
	glBindAttribLocation(prog, 0, "pos");
	glBindAttribLocation(prog, 2, "tex");
		
	glLinkProgram(prog);

	GLint ok;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		 //Some drivers return a size of 1 for an empty log.  This is the size
		  //of a log that contains only a terminating NUL character.
		 
		GLint size;
		GLchar *info = NULL;
		glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &size);
		if (size > 1)
		{
			info = (GLchar *)malloc(size);
			glGetProgramInfoLog(prog, size, NULL, info);
		}

		throw std::runtime_error("failed to link: " + std::string(info ? info : "<empty log>"));
	}

	return prog;
}

static int match_config_to_visual(EGLDisplay egl_display, EGLint visual_id, EGLConfig *configs, int count) {
	int j;
	for (j = 0; j < count; ++j) {
		EGLint id;
		EGLint blue_size, red_size, green_size, alpha_size;
		std::cout << j << "\n";
		if (!eglGetConfigAttrib(egl_display, configs[j], EGL_NATIVE_VISUAL_ID,&id)) 
			continue;
			
		eglGetConfigAttrib(egl_display, configs[j], EGL_RED_SIZE, &red_size);
        eglGetConfigAttrib(egl_display, configs[j], EGL_GREEN_SIZE, &green_size);
        eglGetConfigAttrib(egl_display, configs[j], EGL_BLUE_SIZE, &blue_size);
        eglGetConfigAttrib(egl_display, configs[j], EGL_ALPHA_SIZE, &alpha_size);	
        
        char gbm_format_str[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        memcpy(gbm_format_str, &id, sizeof(EGLint));
        printf("  %d-th GBM format: %s;  sizes(RGBA) = %d,%d,%d,%d,\n",
               j, gbm_format_str, red_size, green_size, blue_size, alpha_size);        
        
		if (id == visual_id) 
			return j;
	}
	return -1;
}

void DrmPreview::findCrtc()
{
	int i;
	drmModeRes *res = drmModeGetResources(drmfd_);
	if (!res)
		throw std::runtime_error("drmModeGetResources failed: " + std::string(ERRSTR));

	if (res->count_crtcs <= 0)
		throw std::runtime_error("drm: no crts");

	max_image_width_ = res->max_width;
	max_image_height_ = res->max_height;

	if (!conId_)
	{
		if (options_->verbose)
			std::cerr << "No connector ID specified.  Choosing default from list:" << std::endl;

		for (i = 0; i < res->count_connectors; i++)
		{
			drmModeConnector *con = drmModeGetConnector(drmfd_, res->connectors[i]);
			drmModeEncoder *enc = NULL;
			drmModeCrtc *crtc = NULL;

			if (con->encoder_id)
			{
				enc = drmModeGetEncoder(drmfd_, con->encoder_id);
				if (enc->crtc_id)
				{
					crtc = drmModeGetCrtc(drmfd_, enc->crtc_id);
				}
			}

			if (!conId_ && crtc)
			{
				conId_ = con->connector_id;
				crtcId_ = crtc->crtc_id;
			}

			if (crtc)
			{
				screen_width_ = crtc->width;
				screen_height_ = crtc->height;
			}

			if (options_->verbose)
				std::cerr << "Connector " << con->connector_id << " (crtc " << (crtc ? crtc->crtc_id : 0) << "): type "
						  << con->connector_type << ", " << (crtc ? crtc->width : 0) << "x" << (crtc ? crtc->height : 0)
						  << (conId_ == (int)con->connector_id ? " (chosen)" : "") << std::endl;
		}

		if (!conId_)
			throw std::runtime_error("No suitable enabled connector found");
	}

	crtcIdx_ = -1;

	for (i = 0; i < res->count_crtcs; ++i)
	{
		if (crtcId_ == res->crtcs[i])
		{
			crtcIdx_ = i;
			break;
		}
	}

	if (crtcIdx_ == -1)
	{
		drmModeFreeResources(res);
		throw std::runtime_error("drm: CRTC " + std::to_string(crtcId_) + " not found");
	}

	if (res->count_connectors <= 0)
	{
		drmModeFreeResources(res);
		throw std::runtime_error("drm: no connectors");
	}

	drmModeConnector *c;
	c = drmModeGetConnector(drmfd_, conId_);
	if (!c)
	{
		drmModeFreeResources(res);
		throw std::runtime_error("drmModeGetConnector failed: " + std::string(ERRSTR));
	}

	if (!c->count_modes)
	{
		drmModeFreeConnector(c);
		drmModeFreeResources(res);
		throw std::runtime_error("connector supports no mode");
	}

	if (options_->fullscreen || width_ == 0 || height_ == 0)
	{
		drmModeCrtc *crtc = drmModeGetCrtc(drmfd_, crtcId_);
		x_ = crtc->x;
		y_ = crtc->y;
		width_ = crtc->width;
		height_ = crtc->height;
		drmModeFreeCrtc(crtc);
	}
	gbm_device = gbm_create_device(drmfd_);
    assert(gbm != NULL);
    
	gbm_surface = gbm_surface_create(gbm_device, width_, height_, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!gbm_surface)
			throw std::runtime_error("Failed to create GBM surface\n");
	
	//egl_display_ = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_MESA, gbm_device, NULL);
    egl_display_ = eglGetDisplay((EGLNativeDisplayType)gbm_device);
    if (egl_display_ == EGL_NO_DISPLAY){
		printf("Failed to get EGL Display 0x%x\n", eglGetError());
	}
	EGLint major, minor;
	if(!eglInitialize(egl_display_, &major, &minor))
		throw std::runtime_error("Failed to get EGL Display\n");
	printf("EGL Version \"%s\"\n", eglQueryString(egl_display_, EGL_VERSION));
	
	eglBindAPI(EGL_OPENGL_API);
	
	static const EGLint attribs[] =
		{
			//EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			//EGL_BUFFER_SIZE, 32,
			//EGL_DEPTH_SIZE, EGL_DONT_CARE,
			//EGL_STENCIL_SIZE, EGL_DONT_CARE,
			EGL_RED_SIZE, 1,
			EGL_GREEN_SIZE, 1,
			EGL_BLUE_SIZE, 1,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_NONE
		};
		
	static const EGLint ctx_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
		
	//EGLConfig config;
	EGLint num_configs;
	EGLConfig *configs;
	int config_index;
	//int i;
	
	if(!eglGetConfigs(egl_display_, NULL, 0, &num_configs) || num_configs < 1){
		printf("cannot get any configs, error: 0x%x\n", eglGetError());
	}
	configs = (EGLConfig*)malloc(num_configs * sizeof(EGLConfig));
	
	if(!configs)
		throw std::runtime_error("no configs");
	
	if (!eglChooseConfig(egl_display_, attribs, configs, num_configs, &num_configs))
		throw std::runtime_error("couldn't get an EGL visual config");
	
	//EGLint vid;
	//if (!eglGetConfigAttrib(egl_display_, config, EGL_NATIVE_VISUAL_ID, &vid))
	//	throw std::runtime_error("eglGetConfigAttrib() failed\n");

	config_index = match_config_to_visual(egl_display_, GBM_FORMAT_XRGB8888, configs, num_configs);
	
	std::cout << config_index << "\n";
	
	//freezes here
		
	egl_context_ = eglCreateContext(egl_display_, configs[config_index], EGL_NO_CONTEXT, ctx_attribs);
	if (!egl_context_){
		printf("eglCreateContext failed 0x%x\n", eglGetError());
		throw std::runtime_error("context failed bro");
	}
		
	printf("im here\n");
		
	egl_surface_ = eglCreateWindowSurface(egl_display_, configs[config_index], (EGLNativeWindowType)gbm_surface, NULL);
	if(egl_surface_ == EGL_NO_SURFACE) {
		printf("failed to create EGL window surface, error: 0x%x\n", eglGetError());
	}
	
	free(configs);
	
	//eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context_);
	//int max_texture_size = 0;
	//glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
	//max_image_width_ = max_image_height_ = max_texture_size;
	// This "undoes" the previous eglMakeCurrent.
	//eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	printf("setup egl i think\n");
}

void DrmPreview::findPlane()
{
	drmModePlaneResPtr planes;
	drmModePlanePtr plane;
	unsigned int i;
	unsigned int j;

	planes = drmModeGetPlaneResources(drmfd_);
	if (!planes)
		throw std::runtime_error("drmModeGetPlaneResources failed: " + std::string(ERRSTR));

	try
	{
		for (i = 0; i < planes->count_planes; ++i)
		{
			plane = drmModeGetPlane(drmfd_, planes->planes[i]);
			if (!planes)
				throw std::runtime_error("drmModeGetPlane failed: " + std::string(ERRSTR));

			if (!(plane->possible_crtcs & (1 << crtcIdx_)))
			{
				drmModeFreePlane(plane);
				continue;
			}

			for (j = 0; j < plane->count_formats; ++j)
			{
				if (plane->formats[j] == out_fourcc_)
				{
					break;
				}
			}

			if (j == plane->count_formats)
			{
				drmModeFreePlane(plane);
				continue;
			}

			planeId_ = plane->plane_id;

			drmModeFreePlane(plane);
			break;
		}
	}
	catch (std::exception const &e)
	{
		drmModeFreePlaneResources(planes);
		throw;
	}

	drmModeFreePlaneResources(planes);
}

DrmPreview::DrmPreview(Options const *options) : Preview(options), last_fd_(-1), first_time_(true)
{
	drmfd_ = drmOpen("vc4", NULL);
	if (drmfd_ < 0)
		throw std::runtime_error("drmOpen failed: " + std::string(ERRSTR));
  
	x_ = options_->preview_x;
	y_ = options_->preview_y;
	width_ = options_->preview_width;
	height_ = options_->preview_height;
	screen_width_ = 0;
	screen_height_ = 0;
	try
	{
		if (!drmIsMaster(drmfd_))
			throw std::runtime_error("DRM preview unavailable - not master");

		conId_ = 0;
		findCrtc();
		out_fourcc_ = DRM_FORMAT_YUV420;
		findPlane();
	}
	catch (std::exception const &e)
	{
		close(drmfd_);
		throw;
	}

	// Default behaviour here is to go fullscreen.
	if (options_->fullscreen || width_ == 0 || height_ == 0 || x_ + width_ > screen_width_ ||
		y_ + height_ > screen_height_)
	{
		x_ = y_ = 0;
		width_ = screen_width_;
		height_ = screen_height_;
	}
}

DrmPreview::~DrmPreview()
{
	close(drmfd_);
}

// DRM doesn't seem to have userspace definitions of its enums, but the properties
// contain enum-name-to-value tables. So the code below ends up using strings and
// searching for name matches. I suppose it works...

static void get_colour_space_info(std::optional<libcamera::ColorSpace> const &cs, char const *&encoding,
								  char const *&range)
{
	static char const encoding_601[] = "601", encoding_709[] = "709";
	static char const range_limited[] = "limited", range_full[] = "full";
	encoding = encoding_601;
	range = range_limited;

	if (cs == libcamera::ColorSpace::Jpeg)
		range = range_full;
	else if (cs == libcamera::ColorSpace::Smpte170m)
		/* all good */;
	else if (cs == libcamera::ColorSpace::Rec709)
		encoding = encoding_709;
	else
		std::cerr << "DrmPreview: unexpected colour space " << libcamera::ColorSpace::toString(cs) << std::endl;
}

static int drm_set_property(int fd, int plane_id, char const *name, char const *val)
{
	drmModeObjectPropertiesPtr properties = nullptr;
	drmModePropertyPtr prop = nullptr;
	int ret = -1;
	properties = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);

	for (unsigned int i = 0; i < properties->count_props; i++)
	{
		int prop_id = properties->props[i];
		prop = drmModeGetProperty(fd, prop_id);
		if (!prop)
			continue;

		if (!drm_property_type_is(prop, DRM_MODE_PROP_ENUM) || !strstr(prop->name, name))
		{
			drmModeFreeProperty(prop);
			prop = nullptr;
			continue;
		}

		// We have found the right property from its name, now search the enum table
		// for the numerical value that corresponds to the value name that we have.
		for (int j = 0; j < prop->count_enums; j++)
		{
			if (!strstr(prop->enums[j].name, val))
				continue;

			ret = drmModeObjectSetProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE, prop_id, prop->enums[j].value);
			if (ret < 0)
				std::cerr << "DrmPreview: failed to set value " << val << " for property " << name << std::endl;
			goto done;
		}

		std::cerr << "DrmPreview: failed to find value " << val << " for property " << name << std::endl;
		goto done;
	}

	std::cerr << "DrmPreview: failed to find property " << name << std::endl;
done:
	if (prop)
		drmModeFreeProperty(prop);
	if (properties)
		drmModeFreeObjectProperties(properties);
	return ret;
}

static void setup_colour_space(int fd, int plane_id, std::optional<libcamera::ColorSpace> const &cs)
{
	char const *encoding, *range;
	get_colour_space_info(cs, encoding, range);

	drm_set_property(fd, plane_id, "COLOR_ENCODING", encoding);
	drm_set_property(fd, plane_id, "COLOR_RANGE", range);
	const char *vs = "#version 300 es\n"
					 "in vec3 pos;\n"
			         "in vec2 tex;\n"
			         "out vec2 texcoord;\n"
			         "\n"
			         "void main() {\n"
			         "  gl_Position = vec4(pos, 1.0);\n"
			         "  texcoord = tex;\n"
			         "}\n";
	GLint vs_s = compile_shader(GL_VERTEX_SHADER, vs);
	const char *fs = "#version 300 es\n"
					 "#extension GL_OES_EGL_image_external : enable\n"
					 "precision mediump float;\n"
					 "uniform samplerExternalOES s;\n"
					 "in vec2 texcoord;\n"
					 "out vec4 out_color;\n"
					 "void main() {\n"
					 "  out_color = texture2D(s, texcoord);\n"
					 "}\n";
	GLint fs_s = compile_shader(GL_FRAGMENT_SHADER, fs);
	GLint prog = link_program(vs_s, fs_s);

	glUseProgram(prog);
	
    std::vector<float> vertices; 
    std::vector<unsigned short> indices;
        
	float N = 100;    //create an NxN grid of triangles (NxNx2 Triangles produced)
    float z = 0;      //empty z component for the POS vector
    
    for (float x = -1, a = 0; x <= 1 && a <= 1; x+= 2/N, a += 1/N)
    {
		for (float y = -1, b = 0; y <= 1 && b <= 1; y+= 2/N, b+= 1/N)
        {
			float theta = atan2(y, x);
			float r = sqrt(x*x + y*y);
			r = r -0.15*pow(r, 3.0) + 0.01*pow(r, 5.0);
			vertices.push_back(r*cos(theta));
			vertices.push_back(r*sin(theta));
			//vertices.push_back(x);
			//vertices.push_back(y);
			vertices.push_back(z);
			vertices.push_back(a);
			vertices.push_back(b);
        }
	}
	 
	for (int x = 0; x < N; x++)
	{
		for (int z = 0; z < N; z++)
		{
			int offset = x * (N+1) + z;
            indices.push_back((short)(offset+0));
            indices.push_back((short)(offset+1));
			indices.push_back((short)(offset+ (N+1) + 1));
            indices.push_back((short)(offset+0));
            indices.push_back((short)(offset+ (N+1)));
            indices.push_back((short)(offset+ (N+1) + 1));
        }
    }
	SSQuad1 = new Mesh({ POS, TEX }, vertices, indices);
}
struct gbm_bo *previous_bo = NULL;
uint32_t previous_fb;

void DrmPreview::makeBuffer(int fd, size_t size, StreamInfo const &info, Buffer &buffer)
{
	if (first_time_)
	{
		first_time_ = false;
		if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_))
			throw std::runtime_error("eglMakeCurrent failed");
		setup_colour_space(drmfd_, planeId_, info.colour_space);
	}

	buffer.fd = fd;
	buffer.size = size;
	buffer.info = info;
	
	if (drmPrimeFDToHandle(drmfd_, fd, &buffer.bo_handle))
		throw std::runtime_error("drmPrimeFDToHandle failed for fd " + std::to_string(fd));

	uint32_t offsets[4] =
		{ 0, info.stride * info.height, info.stride * info.height + (info.stride / 2) * (info.height / 2) };
	uint32_t pitches[4] = { info.stride, info.stride / 2, info.stride / 2 };
	uint32_t bo_handles[4] = { buffer.bo_handle, buffer.bo_handle, buffer.bo_handle };
	
	eglSwapBuffers(egl_display_, egl_surface_);
	bo = gbm_surface_lock_front_buffer(gbm_surface);
	
	std::cout << "bo" << bo << "\n";
	
	if (drmModeAddFB2(drmfd_, info.width, info.height, out_fourcc_, bo_handles, pitches, offsets, &buffer.fb_handle, 0))
		throw std::runtime_error("drmModeAddFB2 failed: " + std::string(ERRSTR));
		
	/*EGLint attribs[50];
	int i = 0;

	attribs[i++] = EGL_WIDTH;
	attribs[i++] = info.width;
	attribs[i++] = EGL_HEIGHT;
	attribs[i++] = info.height;

	attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
	attribs[i++] = DRM_FORMAT_YUV420;
	
	attribs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
	attribs[i++] = fd;

	attribs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
	attribs[i++] = offsets[0];

	attribs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
	attribs[i++] = pitches[0];

	if (pitches[1]) {
		attribs[i++] = EGL_DMA_BUF_PLANE1_FD_EXT;
		attribs[i++] = fd;

		attribs[i++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
		attribs[i++] = offsets[1];
	
		attribs[i++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
		attribs[i++] = pitches[1];
	}

	if (pitches[2]) {
		attribs[i++] = EGL_DMA_BUF_PLANE2_FD_EXT;
		attribs[i++] = fd;

		attribs[i++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
		attribs[i++] = offsets[2];

		attribs[i++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
		attribs[i++] = pitches[2];
	}

	attribs[i++] = EGL_NONE;*/
	//char const *encoding, *range;
	//get_colour_space_info(info.colour_space, encoding, range);	
	EGLint attribs[] = {
		EGL_WIDTH, static_cast<EGLint>(info.width),
		EGL_HEIGHT, static_cast<EGLint>(info.height),
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_YUV420,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(info.stride),
		EGL_DMA_BUF_PLANE1_FD_EXT, fd,
		EGL_DMA_BUF_PLANE1_OFFSET_EXT, static_cast<EGLint>(info.stride * info.height),
		EGL_DMA_BUF_PLANE1_PITCH_EXT, static_cast<EGLint>(info.stride / 2),
		EGL_DMA_BUF_PLANE2_FD_EXT, fd,
		EGL_DMA_BUF_PLANE2_OFFSET_EXT, static_cast<EGLint>(info.stride * info.height + (info.stride / 2) * (info.height / 2)),
		EGL_DMA_BUF_PLANE2_PITCH_EXT, static_cast<EGLint>(info.stride / 2),
		EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC601_EXT,
		EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,
		EGL_NONE
	};
   
	EGLImage image = eglCreateImageKHR(egl_display_,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,NULL, attribs);
	if(!image) {
		printf("failed to create EGL image, error: 0x%x\n", eglGetError());
		throw std::runtime_error("no image made");
	}
	glGenTextures(1, &buffer.texture);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, buffer.texture);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

	eglDestroyImageKHR(egl_display_, image);
	
			
	if (previous_bo){
		drmModeRmFB(drmfd_, previous_fb);
		gbm_surface_release_buffer(gbm_surface, previous_bo);
	}
	
	std::cout << "previous_bo" << previous_bo << "\n";
	
	previous_bo = bo;
	previous_fb = fd;
}

void DrmPreview::Show(int fd, libcamera::Span<uint8_t> span, StreamInfo const &info)
{
	
	Buffer &buffer = buffers_[fd];
	if (buffer.fd == -1){
		makeBuffer(fd, span.size(), info, buffer);
	}
		
	unsigned int x_off = 0, y_off = 0;
	unsigned int w = width_, h = height_;
	if (info.width * height_ > width_ * info.height)
		h = width_ * info.height / info.width, y_off = (height_ - h) / 2;
	else
		w = height_ * info.width / info.height, x_off = (width_ - w) / 2;
	
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, buffer.texture);
	glViewport(0, 0, 960, 1080);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
	SSQuad1->draw();
	glViewport(960, 0, 960, 1080);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	SSQuad1->draw();
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	EGLBoolean success [[maybe_unused]] = eglSwapBuffers(egl_display_, egl_surface_);
	if (drmModeSetPlane(drmfd_, planeId_, crtcId_, buffer.fb_handle, 0, x_off + x_, y_off + y_, w, h, 0, 0,
						buffer.info.width << 16, buffer.info.height << 16))
		throw std::runtime_error("drmModeSetPlane failed: " + std::string(ERRSTR));
		
	if (last_fd_ >= 0)
		done_callback_(last_fd_);
	last_fd_ = fd;
}

void DrmPreview::Reset()
{
	for (auto &it : buffers_){
		drmModeRmFB(drmfd_, it.second.fb_handle);
		glDeleteTextures(1, &it.second.texture);
	}
	buffers_.clear();
	last_fd_ = -1;
	eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	first_time_ = true;
}

Preview *make_drm_preview(Options const *options)
{
	return new DrmPreview(options);
}
