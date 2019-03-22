﻿/*
This Source Code Form is subject to the
terms of the Mozilla Public License, v.
2.0. If a copy of the MPL was not
distributed with this file, You can
obtain one at
http://mozilla.org/MPL/2.0/.
*/

#include "stdafx.h"

#include "renderer.h"
#include "color.h"
#include "Globals.h"
#include "Timer.h"
#include "Train.h"
#include "Camera.h"
#include "simulation.h"
#include "Logs.h"
#include "utilities.h"
#include "simulationtime.h"
#include "application.h"
#include "AnimModel.h"

opengl_renderer GfxRenderer;

int const EU07_PICKBUFFERSIZE{1024}; // size of (square) textures bound with the pick framebuffer

void opengl_light::apply_intensity(float const Factor)
{
	factor = Factor;
}

void opengl_light::apply_angle() {}

void opengl_camera::update_frustum(glm::mat4 const &Projection, glm::mat4 const &Modelview)
{
	m_frustum.calculate(Projection, Modelview);
	// cache inverse tranformation matrix
	// NOTE: transformation is done only to camera-centric space
	m_inversetransformation = glm::inverse(Projection * glm::mat4{glm::mat3{Modelview}});
	// calculate frustum corners
	m_frustumpoints = ndcfrustumshapepoints;
	transform_to_world(std::begin(m_frustumpoints), std::end(m_frustumpoints));
}

// returns true if specified object is within camera frustum, false otherwise
bool opengl_camera::visible(scene::bounding_area const &Area) const
{
	return (m_frustum.sphere_inside(Area.center, Area.radius) > 0.f);
}

bool opengl_camera::visible(TDynamicObject const *Dynamic) const
{
	// sphere test is faster than AABB, so we'll use it here
	glm::vec3 diagonal(static_cast<float>(Dynamic->MoverParameters->Dim.L), static_cast<float>(Dynamic->MoverParameters->Dim.H), static_cast<float>(Dynamic->MoverParameters->Dim.W));
	// we're giving vehicles some extra padding, to allow for things like shared bogeys extending past the main body
	float const radius = glm::length(diagonal) * 0.65f;

	return (m_frustum.sphere_inside(Dynamic->GetPosition(), radius) > 0.0f);
}

// debug helper, draws shape of frustum in world space
void opengl_camera::draw(glm::vec3 const &Offset) const
{
	// m7t port to core gl
}

bool opengl_renderer::Init(GLFWwindow *Window)
{
	if (!Init_caps())
		return false;

	WriteLog("preparing renderer..");

	m_window = Window;

	gl::glsl_common_setup();

	if (true == Global.ScaleSpecularValues)
	{
		m_specularopaquescalefactor = 0.25f;
		m_speculartranslucentscalefactor = 1.5f;
	}

	// rgb value for 5780 kelvin
	Global.DayLight.diffuse[0] = 255.0f / 255.0f;
	Global.DayLight.diffuse[1] = 242.0f / 255.0f;
	Global.DayLight.diffuse[2] = 231.0f / 255.0f;
	Global.DayLight.is_directional = true;
	m_sunlight.id = opengl_renderer::sunlight;

	// create dynamic light pool
	for (int idx = 0; idx < Global.DynamicLightCount; ++idx)
	{
		opengl_light light;
		light.id = 1 + idx;

		light.is_directional = false;

		m_lights.emplace_back(light);
	}
	// preload some common textures
	WriteLog("Loading common gfx data...");
	m_glaretexture = Fetch_Texture("fx/lightglare");
	m_suntexture = Fetch_Texture("fx/sun");
	m_moontexture = Fetch_Texture("fx/moon");
	WriteLog("...gfx data pre-loading done");

	// prepare basic geometry chunks
	float const size = 2.5f / 2.0f;
	auto const geometrybank = m_geometry.create_bank();
	m_billboardgeometry = m_geometry.create_chunk(
	    gfx::vertex_array{
	        {{-size, size, 0.f}, glm::vec3(), {1.f, 1.f}}, {{size, size, 0.f}, glm::vec3(), {0.f, 1.f}}, {{-size, -size, 0.f}, glm::vec3(), {1.f, 0.f}}, {{size, -size, 0.f}, glm::vec3(), {0.f, 0.f}}},
	    geometrybank, GL_TRIANGLE_STRIP);
	m_empty_vao = std::make_unique<gl::vao>();

	try
	{
		m_vertex_shader = std::make_unique<gl::shader>("vertex.vert");
		m_line_shader = make_shader("traction.vert", "traction.frag");
		m_freespot_shader = make_shader("freespot.vert", "freespot.frag");
		m_shadow_shader = make_shader("simpleuv.vert", "shadowmap.frag");
		m_alpha_shadow_shader = make_shader("simpleuv.vert", "alphashadowmap.frag");
		m_pick_shader = make_shader("vertexonly.vert", "pick.frag");
		m_billboard_shader = make_shader("simpleuv.vert", "billboard.frag");
		m_celestial_shader = make_shader("celestial.vert", "celestial.frag");
		if (Global.gfx_usegles)
			m_depth_pointer_shader = make_shader("quad.vert", "gles_depthpointer.frag");
		m_invalid_material = Fetch_Material("invalid");
	}
	catch (gl::shader_exception const &e)
	{
		ErrorLog("invalid shader: " + std::string(e.what()));
		return false;
	}

	scene_ubo = std::make_unique<gl::ubo>(sizeof(gl::scene_ubs), 0);
	model_ubo = std::make_unique<gl::ubo>(sizeof(gl::model_ubs), 1, GL_STREAM_DRAW);
	light_ubo = std::make_unique<gl::ubo>(sizeof(gl::light_ubs), 2);

	// better initialize with 0 to not crash driver/whole system
	// when we forget
	memset(&light_ubs, 0, sizeof(light_ubs));
	memset(&model_ubs, 0, sizeof(model_ubs));
	memset(&scene_ubs, 0, sizeof(scene_ubs));

	light_ubo->update(light_ubs);
	model_ubo->update(model_ubs);
	scene_ubo->update(scene_ubs);

	int samples = 1 << Global.iMultisampling;
	if (!Global.gfx_usegles && samples > 1)
		glEnable(GL_MULTISAMPLE);

	m_pfx_motionblur = std::make_unique<gl::postfx>("motionblur");
	m_pfx_tonemapping = std::make_unique<gl::postfx>("tonemapping");

	m_empty_cubemap = std::make_unique<gl::cubemap>();
	m_empty_cubemap->alloc(Global.gfx_format_color, 16, 16, GL_RGB, GL_FLOAT);

	m_viewports.push_back(std::make_unique<viewport_config>());
	viewport_config &default_viewport = *m_viewports.front().get();
	default_viewport.width = Global.gfx_framebuffer_width;
	default_viewport.height = Global.gfx_framebuffer_height;
	default_viewport.main = true;
	default_viewport.window = m_window;
	default_viewport.draw_range = 1.0f;

	if (!init_viewport(default_viewport))
		return false;
	glfwMakeContextCurrent(m_window);
	gl::buffer::unbind();

	if (Global.gfx_shadowmap_enabled)
	{
		m_shadow_fb = std::make_unique<gl::framebuffer>();
		m_shadow_tex = std::make_unique<opengl_texture>();
		m_shadow_tex->alloc_rendertarget(Global.gfx_format_depth, GL_DEPTH_COMPONENT, m_shadowbuffersize, m_shadowbuffersize);
		m_shadow_fb->attach(*m_shadow_tex, GL_DEPTH_ATTACHMENT);

		if (!m_shadow_fb->is_complete())
			return false;

		WriteLog("shadows enabled");

		m_cabshadows_fb = std::make_unique<gl::framebuffer>();
		m_cabshadows_tex = std::make_unique<opengl_texture>();
		m_cabshadows_tex->alloc_rendertarget(Global.gfx_format_depth, GL_DEPTH_COMPONENT, m_shadowbuffersize, m_shadowbuffersize);
		m_cabshadows_fb->attach(*m_cabshadows_tex, GL_DEPTH_ATTACHMENT);

		if (!m_cabshadows_fb->is_complete())
			return false;

		WriteLog("cabshadows enabled");
	}

	if (Global.gfx_envmap_enabled)
	{
		m_env_rb = std::make_unique<gl::renderbuffer>();
		m_env_rb->alloc(Global.gfx_format_depth, gl::ENVMAP_SIZE, gl::ENVMAP_SIZE);
		m_env_tex = std::make_unique<gl::cubemap>();
		m_env_tex->alloc(Global.gfx_format_color, gl::ENVMAP_SIZE, gl::ENVMAP_SIZE, GL_RGB, GL_FLOAT);

		m_env_fb = std::make_unique<gl::framebuffer>();

		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		for (int i = 0; i < 6; i++)
		{
			m_env_fb->attach(*m_empty_cubemap, i, GL_COLOR_ATTACHMENT0);
			m_env_fb->clear(GL_COLOR_BUFFER_BIT);
		}

		m_env_fb->detach(GL_COLOR_ATTACHMENT0);
		m_env_fb->attach(*m_env_rb, GL_DEPTH_ATTACHMENT);

		WriteLog("envmap enabled");
	}

	m_pick_tex = std::make_unique<opengl_texture>();
	m_pick_tex->alloc_rendertarget(GL_RGB8, GL_RGB, EU07_PICKBUFFERSIZE, EU07_PICKBUFFERSIZE);
	m_pick_rb = std::make_unique<gl::renderbuffer>();
	m_pick_rb->alloc(Global.gfx_format_depth, EU07_PICKBUFFERSIZE, EU07_PICKBUFFERSIZE);
	m_pick_fb = std::make_unique<gl::framebuffer>();
	m_pick_fb->attach(*m_pick_tex, GL_COLOR_ATTACHMENT0);
	m_pick_fb->attach(*m_pick_rb, GL_DEPTH_ATTACHMENT);

	if (!m_pick_fb->is_complete())
		return false;

	m_picking_pbo = std::make_unique<gl::pbo>();
	m_picking_node_pbo = std::make_unique<gl::pbo>();

	m_depth_pointer_pbo = std::make_unique<gl::pbo>();
	if (!Global.gfx_usegles && Global.iMultisampling)
	{
		m_depth_pointer_rb = std::make_unique<gl::renderbuffer>();
		m_depth_pointer_rb->alloc(Global.gfx_format_depth, 1, 1);

		m_depth_pointer_fb = std::make_unique<gl::framebuffer>();
		m_depth_pointer_fb->attach(*m_depth_pointer_rb, GL_DEPTH_ATTACHMENT);

		if (!m_depth_pointer_fb->is_complete())
			return false;
	}
	else if (Global.gfx_usegles)
	{
		m_depth_pointer_tex = std::make_unique<opengl_texture>();

		GLenum format = Global.gfx_format_depth;
		if (Global.gfx_skippipeline)
		{
			gl::framebuffer::unbind();
			GLint bits, type;
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &bits);
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &type);
			if (type == GL_FLOAT && bits == 32)
				format = GL_DEPTH_COMPONENT32F;
			else if (bits == 16)
				format = GL_DEPTH_COMPONENT16;
			else if (bits == 24)
				format = GL_DEPTH_COMPONENT24;
			else if (bits == 32)
				format = GL_DEPTH_COMPONENT32;
		}

		m_depth_pointer_tex->alloc_rendertarget(format, GL_DEPTH_COMPONENT, Global.gfx_framebuffer_width, Global.gfx_framebuffer_height);

		m_depth_pointer_rb = std::make_unique<gl::renderbuffer>();
		m_depth_pointer_rb->alloc(GL_R16UI, Global.gfx_framebuffer_width, Global.gfx_framebuffer_height);

		m_depth_pointer_fb = std::make_unique<gl::framebuffer>();
		m_depth_pointer_fb->attach(*m_depth_pointer_tex, GL_DEPTH_ATTACHMENT);

		m_depth_pointer_fb2 = std::make_unique<gl::framebuffer>();
		m_depth_pointer_fb2->attach(*m_depth_pointer_rb, GL_COLOR_ATTACHMENT0);

		if (!m_depth_pointer_fb->is_complete())
			return false;

		if (!m_depth_pointer_fb2->is_complete())
			return false;
	}

	WriteLog("picking objects created");

	WriteLog("renderer initialization finished!");

	return true;
}

bool opengl_renderer::AddViewport(const global_settings::extraviewport_config &conf)
{
	m_viewports.push_back(std::make_unique<viewport_config>());
	viewport_config &vp = *m_viewports.back().get();
	vp.width = conf.width;
	vp.height = conf.height;
	vp.window = Application.window(-1, true, vp.width, vp.height, Application.find_monitor(conf.monitor));
	vp.camera_transform = conf.transform;
	vp.draw_range = conf.draw_range;

	bool ret = init_viewport(vp);
	glfwMakeContextCurrent(m_window);
	gl::buffer::unbind();

	return ret;
}

bool opengl_renderer::init_viewport(viewport_config &vp)
{
	glfwMakeContextCurrent(vp.window);

	WriteLog("init viewport: " + std::to_string(vp.width) + ", " + std::to_string(vp.height));

	glfwSwapInterval( Global.VSync ? 1 : 0 );

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);

	glClearColor(51.0f / 255.0f, 102.0f / 255.0f, 85.0f / 255.0f, 1.0f); // initial background Color

	glFrontFace(GL_CCW);
	glEnable(GL_CULL_FACE);

	glEnable(GL_DEPTH_TEST);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_BLEND);

	if (!Global.gfx_usegles)
		glClearDepth(0.0f);
	else
		glClearDepthf(0.0f);

	glDepthFunc(GL_GEQUAL);

	if (GLAD_GL_ARB_clip_control)
		glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
	else if (GLAD_GL_EXT_clip_control)
		glClipControlEXT(GL_LOWER_LEFT_EXT, GL_ZERO_TO_ONE_EXT);

	if (!Global.gfx_usegles)
		glEnable(GL_PROGRAM_POINT_SIZE);

	{
		GLuint v;
		glGenVertexArrays(1, &v);
		glBindVertexArray(v);
	}

	int samples = 1 << Global.iMultisampling;

	model_ubo->bind_uniform();
	scene_ubo->bind_uniform();
	light_ubo->bind_uniform();

	if (!Global.gfx_skippipeline)
	{
		vp.msaa_rbc = std::make_unique<gl::renderbuffer>();
		vp.msaa_rbc->alloc(Global.gfx_format_color, vp.width, vp.height, samples);

		vp.msaa_rbd = std::make_unique<gl::renderbuffer>();
		vp.msaa_rbd->alloc(Global.gfx_format_depth, vp.width, vp.height, samples);

		vp.msaa_fb = std::make_unique<gl::framebuffer>();
		vp.msaa_fb->attach(*vp.msaa_rbc, GL_COLOR_ATTACHMENT0);
		vp.msaa_fb->attach(*vp.msaa_rbd, GL_DEPTH_ATTACHMENT);

		if (Global.gfx_postfx_motionblur_enabled)
		{
			vp.msaa_rbv = std::make_unique<gl::renderbuffer>();
			vp.msaa_rbv->alloc(Global.gfx_postfx_motionblur_format, vp.width, vp.height, samples);
			vp.msaa_fb->attach(*vp.msaa_rbv, GL_COLOR_ATTACHMENT1);

			vp.main_tex = std::make_unique<opengl_texture>();
			vp.main_tex->alloc_rendertarget(Global.gfx_format_color, GL_RGB, vp.width, vp.height, 1, GL_CLAMP_TO_EDGE);

			vp.main_fb = std::make_unique<gl::framebuffer>();
			vp.main_fb->attach(*vp.main_tex, GL_COLOR_ATTACHMENT0);

			vp.main_texv = std::make_unique<opengl_texture>();
			vp.main_texv->alloc_rendertarget(Global.gfx_postfx_motionblur_format, GL_RG, vp.width, vp.height);
			vp.main_fb->attach(*vp.main_texv, GL_COLOR_ATTACHMENT1);
			vp.main_fb->setup_drawing(2);

			if (!vp.main_fb->is_complete())
				return false;

			WriteLog("motion blur enabled");
		}

		if (!vp.msaa_fb->is_complete())
			return false;

		vp.main2_tex = std::make_unique<opengl_texture>();
		vp.main2_tex->alloc_rendertarget(Global.gfx_format_color, GL_RGB, vp.width, vp.height);

		vp.main2_fb = std::make_unique<gl::framebuffer>();
		vp.main2_fb->attach(*vp.main2_tex, GL_COLOR_ATTACHMENT0);
		if (!vp.main2_fb->is_complete())
			return false;
	}

	return true;
}

std::unique_ptr<gl::program> opengl_renderer::make_shader(std::string v, std::string f)
{
	gl::shader vert(v);
	gl::shader frag(f);
	gl::program *prog = new gl::program({vert, frag});
	return std::unique_ptr<gl::program>(prog);
}

bool opengl_renderer::Render()
{
	Timer::subsystem.gfx_total.start();

	GLuint gl_time_ready;
	if (!Global.gfx_usegles)
	{
		gl_time_ready = 0;
		if (m_gltimequery)
		{
			glGetQueryObjectuiv(m_gltimequery, GL_QUERY_RESULT_AVAILABLE, &gl_time_ready);
			if (gl_time_ready)
				glGetQueryObjectui64v(m_gltimequery, GL_QUERY_RESULT, &m_gllasttime);
		}
		else
		{
			glGenQueries(1, &m_gltimequery);
			gl_time_ready = 1;
		}

		if (gl_time_ready)
			glBeginQuery(GL_TIME_ELAPSED, m_gltimequery);
	}

	// fetch simulation data
	if (simulation::is_ready)
	{
		m_sunlight = Global.DayLight;
		// quantize sun angle to reduce shadow crawl
		auto const quantizationstep{0.004f};
		m_sunlight.direction = glm::normalize(quantizationstep * glm::roundEven(m_sunlight.direction * (1.f / quantizationstep)));
	}
	// generate new frame
	m_renderpass.draw_mode = rendermode::none; // force setup anew
	m_debugstats = debug_stats();

	for (auto &viewport : m_viewports) {
		Render_pass(*viewport, rendermode::color);
	}

	glfwMakeContextCurrent(m_window);
	gl::buffer::unbind();
	m_current_viewport = &(*m_viewports.front());

	m_drawcount = m_cellqueue.size();
	m_debugtimestext.clear();
	m_debugtimestext += "cpu: " + to_string(Timer::subsystem.gfx_color.average(), 2) + " ms (" + std::to_string(m_cellqueue.size()) + " sectors)\n" +=
	    "cpu swap: " + to_string(Timer::subsystem.gfx_swap.average(), 2) + " ms\n" += "uilayer: " + to_string(Timer::subsystem.gfx_gui.average(), 2) + "ms\n" +=
	    "mainloop total: " + to_string(Timer::subsystem.mainloop_total.average(), 2) + "ms\n";

	if (!Global.gfx_usegles)
	{
		if (gl_time_ready)
			glEndQuery(GL_TIME_ELAPSED);

		if (m_gllasttime)
			m_debugtimestext += "gpu: " + to_string((double)(m_gllasttime / 1000ULL) / 1000.0, 3) + "ms";
	}

	m_debugstatstext = "drawcalls:  " + to_string(m_debugstats.drawcalls) + "\n" + " vehicles:  " + to_string(m_debugstats.dynamics) + "\n" + " models:    " + to_string(m_debugstats.models) + "\n" +
	                   " submodels: " + to_string(m_debugstats.submodels) + "\n" + " paths:     " + to_string(m_debugstats.paths) + "\n" + " shapes:    " + to_string(m_debugstats.shapes) + "\n" +
	                   " traction:  " + to_string(m_debugstats.traction) + "\n" + " lines:     " + to_string(m_debugstats.lines);

	if (DebugModeFlag)
		m_debugtimestext += m_textures.info();

	++m_framestamp;

	Timer::subsystem.gfx_total.stop();

	return true; // for now always succeed
}

void opengl_renderer::SwapBuffers()
{
	Timer::subsystem.gfx_swap.start();

	for (auto &viewport : m_viewports) {
		if (viewport->window)
			glfwSwapBuffers(viewport->window);
	}

	// swapbuffers() could unbind current buffers so we prepare for it on our end
	gfx::opengl_vbogeometrybank::reset();
	Timer::subsystem.gfx_swap.stop();
}

void opengl_renderer::draw_debug_ui()
{
	if (!debug_ui_active)
		return;

	if (ImGui::Begin("headlight config", &debug_ui_active))
	{
		ImGui::SetWindowSize(ImVec2(0, 0));

		headlight_config_s &conf = headlight_config;
		ImGui::SliderFloat("in_cutoff", &conf.in_cutoff, 0.9f, 1.1f);
		ImGui::SliderFloat("out_cutoff", &conf.out_cutoff, 0.9f, 1.1f);

		ImGui::SliderFloat("falloff_linear", &conf.falloff_linear, 0.0f, 1.0f, "%.3f", 2.0f);
		ImGui::SliderFloat("falloff_quadratic", &conf.falloff_quadratic, 0.0f, 1.0f, "%.3f", 2.0f);

		ImGui::SliderFloat("ambient", &conf.ambient, 0.0f, 3.0f);
		ImGui::SliderFloat("intensity", &conf.intensity, 0.0f, 10.0f);
	}
	ImGui::End();
}

// runs jobs needed to generate graphics for specified render pass
void opengl_renderer::Render_pass(viewport_config &vp, rendermode const Mode)
{
	setup_pass(vp, m_renderpass, Mode);
	switch (m_renderpass.draw_mode)
	{

	case rendermode::color:
	{
		glDebug("rendermode::color");

		glDebug("context switch");
		glfwMakeContextCurrent(vp.window);
		gl::buffer::unbind();
		m_current_viewport = &vp;

		if (!simulation::is_ready)
		{
			gl::framebuffer::unbind();
			glClearColor(0.0f, 0.5f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			if (vp.main)
				Application.render_ui();
			break;
		}

		scene_ubs.time = Timer::GetTime();
		scene_ubs.projection = OpenGLMatrices.data(GL_PROJECTION);
		scene_ubo->update(scene_ubs);
		scene_ubo->bind_uniform();

		m_colorpass = m_renderpass;

		if (!Global.gfx_usegles)
		{
			if (Global.bWireFrame)
				glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
			else
				glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		}

		setup_shadow_map(nullptr, m_renderpass);
		setup_env_map(nullptr);

		if (Global.gfx_shadowmap_enabled && vp.main)
		{
			glDebug("render shadowmap start");
			Timer::subsystem.gfx_shadows.start();

			Render_pass(vp, rendermode::shadows);
			if (!FreeFlyModeFlag && Global.render_cab)
				Render_pass(vp, rendermode::cabshadows);
			setup_pass(vp, m_renderpass, Mode); // restore draw mode. TBD, TODO: render mode stack

			Timer::subsystem.gfx_shadows.stop();
			glDebug("render shadowmap end");
		}

		if (Global.gfx_envmap_enabled && vp.main)
		{
			// potentially update environmental cube map
			if (Render_reflections(vp))
				setup_pass(vp, m_renderpass, Mode); // restore color pass settings
			setup_env_map(m_env_tex.get());
		}

		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

		setup_drawing(false);

		glm::ivec2 target_size(vp.width, vp.height);
		if (vp.main) // TODO: update window sizes also for extra viewports
			target_size = glm::ivec2(Global.iWindowWidth, Global.iWindowHeight);

		if (!Global.gfx_skippipeline)
		{
			vp.msaa_fb->bind();

			if (Global.gfx_postfx_motionblur_enabled)
				vp.msaa_fb->setup_drawing(2);
			else
				vp.msaa_fb->setup_drawing(1);

			glViewport(0, 0, vp.width, vp.height);

			vp.msaa_fb->clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		}
		else
		{
			if (!Global.gfx_usegles && !Global.gfx_shadergamma)
				glEnable(GL_FRAMEBUFFER_SRGB);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glViewport(0, 0, target_size.x, target_size.y);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		}

		glEnable(GL_DEPTH_TEST);

		Timer::subsystem.gfx_color.start();
		setup_matrices();
		setup_drawing(true);

		glm::mat4 future;
		if (Global.pCamera.m_owner != nullptr)
		{
			auto const *vehicle = Global.pCamera.m_owner;
			glm::mat4 mv = OpenGLMatrices.data(GL_MODELVIEW);
			future = glm::translate(mv, -glm::vec3(vehicle->get_future_movement())) * glm::inverse(mv);
		}

		model_ubs.future = glm::mat4();

		glDebug("render environment");

		scene_ubs.projection = OpenGLMatrices.data(GL_PROJECTION);
		scene_ubo->update(scene_ubs);
		Render(&simulation::Environment);

		// opaque parts...
		setup_drawing(false);

		// without rain/snow we can render the cab early to limit the overdraw
		// precipitation happens when overcast is in 1-2 range
		if (!FreeFlyModeFlag && Global.Overcast <= 1.0f && Global.render_cab)
		{
			glDebug("render cab opaque");
			if (Global.gfx_shadowmap_enabled)
				setup_shadow_map(m_cabshadows_tex.get(), m_cabshadowpass);

			auto const *vehicle = simulation::Train->Dynamic();
			Render_cab(vehicle, vehicle->InteriorLightLevel, false);
		}

		glDebug("render opaque region");

		model_ubs.future = future;

		if (Global.gfx_shadowmap_enabled)
			setup_shadow_map(m_shadow_tex.get(), m_shadowpass);

		Render(simulation::Region);

		// ...translucent parts
		glDebug("render translucent region");
		setup_drawing(true);
		Render_Alpha(simulation::Region);

		// precipitation; done at end, only before cab render
		Render_precipitation();

		// cab render
		if (false == FreeFlyModeFlag && Global.render_cab)
		{
			glDebug("render translucent cab");
			model_ubs.future = glm::mat4();
			if (Global.gfx_shadowmap_enabled)
				setup_shadow_map(m_cabshadows_tex.get(), m_cabshadowpass);
			// cache shadow colour in case we need to account for cab light
			auto const *vehicle{simulation::Train->Dynamic()};
			if (Global.Overcast > 1.0f)
			{
				// with active precipitation draw the opaque cab parts here to mask rain/snow placed 'inside' the cab
				setup_drawing(false);
				Render_cab(vehicle, vehicle->InteriorLightLevel, false);
				setup_drawing(true);
			}
			Render_cab(vehicle, vehicle->InteriorLightLevel, true);
		}

		Timer::subsystem.gfx_color.stop();

		setup_shadow_map(nullptr, m_renderpass);
		setup_env_map(nullptr);

		if (!Global.gfx_usegles)
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

		if (!Global.gfx_skippipeline)
		{
			if (Global.gfx_postfx_motionblur_enabled)
			{
				vp.main_fb->clear(GL_COLOR_BUFFER_BIT);
				vp.msaa_fb->blit_to(vp.main_fb.get(), vp.width, vp.height, GL_COLOR_BUFFER_BIT, GL_COLOR_ATTACHMENT0);
				vp.msaa_fb->blit_to(vp.main_fb.get(), vp.width, vp.height, GL_COLOR_BUFFER_BIT, GL_COLOR_ATTACHMENT1);

				model_ubs.param[0].x = m_framerate / (1.0 / Global.gfx_postfx_motionblur_shutter);
				model_ubo->update(model_ubs);
				m_pfx_motionblur->apply({vp.main_tex.get(), vp.main_texv.get()}, vp.main2_fb.get());
			}
			else
			{
				vp.main2_fb->clear(GL_COLOR_BUFFER_BIT);
				vp.msaa_fb->blit_to(vp.main2_fb.get(), vp.width, vp.height, GL_COLOR_BUFFER_BIT, GL_COLOR_ATTACHMENT0);
			}

			if (!Global.gfx_usegles && !Global.gfx_shadergamma)
				glEnable(GL_FRAMEBUFFER_SRGB);

			glViewport(0, 0, target_size.x, target_size.y);
			m_pfx_tonemapping->apply(*vp.main2_tex, nullptr);
			opengl_texture::reset_unit_cache();
		}

		if (!Global.gfx_usegles && !Global.gfx_shadergamma)
			glDisable(GL_FRAMEBUFFER_SRGB);

		glDebug("uilayer render");
		Timer::subsystem.gfx_gui.start();

		if (vp.main) {
			draw_debug_ui();
			Application.render_ui();
		}

		Timer::subsystem.gfx_gui.stop();

		// restore binding
		scene_ubo->bind_uniform();

		glDebug("rendermode::color end");
		break;
	}

	case rendermode::shadows:
	{
		if (!simulation::is_ready)
			break;

		glDebug("rendermode::shadows");

		glPolygonOffset(1.0f, 1.0f);
		glEnable(GL_POLYGON_OFFSET_FILL);

		glEnable(GL_DEPTH_TEST);

		glViewport(0, 0, m_shadowbuffersize, m_shadowbuffersize);
		m_shadow_fb->bind();
		m_shadow_fb->clear(GL_DEPTH_BUFFER_BIT);

		setup_matrices();
		setup_drawing(false);

		scene_ubs.projection = OpenGLMatrices.data(GL_PROJECTION);
		scene_ubo->update(scene_ubs);
		Render(simulation::Region);

		// setup_drawing(true);
		// glDepthMask(GL_TRUE);
		// Render_Alpha(simulation::Region);

		m_shadowpass = m_renderpass;

		glDisable(GL_POLYGON_OFFSET_FILL);

		m_shadow_fb->unbind();

		glDebug("rendermodeshadows ::end");

		break;
	}

	case rendermode::cabshadows:
	{
		glDebug("rendermode::cabshadows");

		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(1.0f, 1.0f);

		glEnable(GL_DEPTH_TEST);

		glViewport(0, 0, m_shadowbuffersize, m_shadowbuffersize);
		m_cabshadows_fb->bind();
		m_cabshadows_fb->clear(GL_DEPTH_BUFFER_BIT);

		setup_matrices();
		setup_drawing(false);

		scene_ubs.projection = OpenGLMatrices.data(GL_PROJECTION);
		scene_ubo->update(scene_ubs);

		Render_cab(simulation::Train->Dynamic(), 0.0f, false);
		Render_cab(simulation::Train->Dynamic(), 0.0f, true);
		m_cabshadowpass = m_renderpass;

		glDisable(GL_POLYGON_OFFSET_FILL);

		m_cabshadows_fb->unbind();

		glDebug("rendermode::cabshadows end");

		break;
	}

	case rendermode::reflections:
	{
		if (!simulation::is_ready)
			break;

		glDebug("rendermode::reflections");

		// NOTE: buffer attachment and viewport setup in this mode is handled by the wrapper method
		glEnable(GL_DEPTH_TEST);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		m_env_fb->clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		m_env_fb->bind();

		setup_env_map(m_empty_cubemap.get());

		setup_matrices();
		setup_shadow_map(nullptr, m_renderpass);

		// render
		setup_drawing(true);

		scene_ubs.projection = OpenGLMatrices.data(GL_PROJECTION);
		scene_ubo->update(scene_ubs);
		Render(&simulation::Environment);

		// opaque parts...
		setup_drawing(false);
		setup_shadow_map(m_shadow_tex.get(), m_shadowpass);

		scene_ubs.projection = OpenGLMatrices.data(GL_PROJECTION);
		scene_ubo->update(scene_ubs);
		Render(simulation::Region);

		m_env_fb->unbind();

		glDebug("rendermode::reflections end");

		break;
	}

	case rendermode::pickcontrols:
	{
		if (!simulation::is_ready || !simulation::Train)
			break;

		glDebug("rendermode::pickcontrols");

		glEnable(GL_DEPTH_TEST);
		glViewport(0, 0, EU07_PICKBUFFERSIZE, EU07_PICKBUFFERSIZE);
		m_pick_fb->bind();
		m_pick_fb->clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		m_pickcontrolsitems.clear();
		setup_matrices();
		setup_drawing(false);

		scene_ubs.projection = OpenGLMatrices.data(GL_PROJECTION);
		scene_ubo->update(scene_ubs);
		if (simulation::Train != nullptr) {
			Render_cab(simulation::Train->Dynamic(), 0.0f);
			Render(simulation::Train->Dynamic());
		}

		m_pick_fb->unbind();

		glDebug("rendermode::pickcontrols end");
		break;
	}

	case rendermode::pickscenery:
	{
		if (!simulation::is_ready)
			break;

		glEnable(GL_DEPTH_TEST);
		glViewport(0, 0, EU07_PICKBUFFERSIZE, EU07_PICKBUFFERSIZE);
		m_pick_fb->bind();
		m_pick_fb->clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		m_picksceneryitems.clear();
		setup_matrices();
		setup_drawing(false);

		scene_ubs.projection = OpenGLMatrices.data(GL_PROJECTION);
		scene_ubo->update(scene_ubs);
		Render(simulation::Region);

		break;
	}

	default:
	{
		break;
	}
	}
}

// creates dynamic environment cubemap
bool opengl_renderer::Render_reflections(viewport_config &vp)
{
	if (Global.ReflectionUpdatesPerSecond == 0)
		return false;

	auto const &time = simulation::Time.data();
	auto const timestamp = time.wMilliseconds + time.wSecond * 1000 + time.wMinute * 1000 * 60 + time.wHour * 1000 * 60 * 60;

	if ((timestamp - m_environmentupdatetime < Global.ReflectionUpdatesPerSecond)
	        && (glm::length(m_renderpass.pass_camera.position() - m_environmentupdatelocation) < 1000.0))
	{
		// run update every 5+ mins of simulation time, or at least 1km from the last location
		return false;
	}
	m_environmentupdatetime = timestamp;
	m_environmentupdatelocation = m_renderpass.pass_camera.position();

	glViewport(0, 0, gl::ENVMAP_SIZE, gl::ENVMAP_SIZE);
	for (m_environmentcubetextureface = 0; m_environmentcubetextureface < 6; ++m_environmentcubetextureface)
	{
		m_env_fb->attach(*m_env_tex, m_environmentcubetextureface, GL_COLOR_ATTACHMENT0);
		if (m_env_fb->is_complete())
			Render_pass(vp, rendermode::reflections);
	}
	m_env_tex->generate_mipmaps();
	m_env_fb->detach(GL_COLOR_ATTACHMENT0);

	return true;
}

glm::mat4 opengl_renderer::perspective_projection(float fovy, float aspect, float znear, float zfar)
{
	if (GLAD_GL_ARB_clip_control || GLAD_GL_EXT_clip_control)
	{
		const float f = 1.0f / tan(fovy / 2.0f);

		// when clip_control available, use projection matrix with 1..0 Z range and infinite zfar
		return glm::mat4( //
		    f / aspect, 0.0f, 0.0f, 0.0f, //
		    0.0f, f, 0.0f, 0.0f, //
		    0.0f, 0.0f, 0.0f, -1.0f, //
		    0.0f, 0.0f, znear, 0.0f //
		);
	}
	else
		// or use standard matrix but with 1..-1 Z range
		// (reverse Z don't give any extra precision without clip_control, but it is used anyway for consistency)
		return glm::mat4( //
		           1.0f, 0.0f, 0.0f, 0.0f, //
		           0.0f, 1.0f, 0.0f, 0.0f, //
		           0.0f, 0.0f, -1.0f, 0.0f, //
		           0.0f, 0.0f, 0.0f, 1.0f //
		           ) *
		       glm::perspective(fovy, aspect, znear, zfar);
}

glm::mat4 opengl_renderer::perpsective_frustumtest_projection(float fovy, float aspect, float znear, float zfar)
{
	// for frustum calculation, use standard opengl matrix
	return glm::perspective(fovy, aspect, znear, zfar);
}

glm::mat4 opengl_renderer::ortho_projection(float l, float r, float b, float t, float znear, float zfar)
{
	glm::mat4 proj = glm::ortho(l, r, b, t, znear, zfar);
	if (GLAD_GL_ARB_clip_control || GLAD_GL_EXT_clip_control)
		// when clip_control available, use projection matrix with 1..0 Z range
		return glm::mat4( //
		           1.0f, 0.0f, 0.0f, 0.0f, //
		           0.0f, 1.0f, 0.0f, 0.0f, //
		           0.0f, 0.0f, -0.5f, 0.0f, //
		           0.0f, 0.0f, 0.5f, 1.0f //
		           ) *
		       proj;
	else
		// or use standard matrix but with 1..-1 Z range
		// (reverse Z don't give any extra precision without clip_control, but it is used anyway for consistency)
		return glm::mat4( //
		           1.0f, 0.0f, 0.0f, 0.0f, //
		           0.0f, 1.0f, 0.0f, 0.0f, //
		           0.0f, 0.0f, -1.0f, 0.0f, //
		           0.0f, 0.0f, 0.0f, 1.0f //
		           ) *
		       proj;
}

glm::mat4 opengl_renderer::ortho_frustumtest_projection(float l, float r, float b, float t, float znear, float zfar)
{
	// for frustum calculation, use standard opengl matrix
	return glm::ortho(l, r, b, t, znear, zfar);
}

void opengl_renderer::setup_pass(viewport_config &Viewport, renderpass_config &Config, rendermode const Mode,
                                 float const Znear, float const Zfar, bool const Ignoredebug)
{

	Config.draw_mode = Mode;

	if (false == simulation::is_ready)
	{
		return;
	}
	// setup draw range
	switch (Mode)
	{
	case rendermode::color:
	{
		Config.draw_range = Global.BaseDrawRange;
		break;
	}
	case rendermode::shadows:
	{
		Config.draw_range = Global.BaseDrawRange * 0.5f;
		break;
	}
	case rendermode::cabshadows:
	{
		Config.draw_range = simulation::Train->Occupied()->Dim.L;
		break;
	}
	case rendermode::reflections:
	{
		Config.draw_range = Global.BaseDrawRange;
		break;
	}
	case rendermode::pickcontrols:
	{
		Config.draw_range = 50.f;
		break;
	}
	case rendermode::pickscenery:
	{
		Config.draw_range = Global.BaseDrawRange * 0.5f;
		break;
	}
	default:
	{
		Config.draw_range = 0.f;
		break;
	}
	}

	Config.draw_range *= Viewport.draw_range;

	// setup camera
	auto &camera = Config.pass_camera;

	glm::dmat4 viewmatrix(1.0);

	glm::mat4 frustumtest_proj;

	glm::ivec2 target_size(Viewport.width, Viewport.height);
	if (Viewport.main) // TODO: update window sizes also for extra viewports
		target_size = glm::ivec2(Global.iWindowWidth, Global.iWindowHeight);

	float const fovy = glm::radians(Global.FieldOfView / Global.ZoomFactor);
	float const aspect = (float)target_size.x / std::max(1.f, (float)target_size.y);

	Config.viewport_camera.position() = Global.pCamera.Pos;

	switch (Mode)
	{
	case rendermode::color:
	{
		viewmatrix = glm::dmat4(Viewport.camera_transform);

		// modelview
		if ((false == DebugCameraFlag) || (true == Ignoredebug))
		{
			camera.position() = Global.pCamera.Pos;
			Global.pCamera.SetMatrix(viewmatrix);
		}
		else
		{
			camera.position() = Global.pDebugCamera.Pos;
			Global.pDebugCamera.SetMatrix(viewmatrix);
		}

		// projection
		auto const zfar = Config.draw_range * Global.fDistanceFactor * Zfar;
		auto const znear = (Znear > 0.f ? Znear * zfar : 0.1f * Global.ZoomFactor);

		camera.projection() = perspective_projection(fovy, aspect, znear, zfar);
		frustumtest_proj = perpsective_frustumtest_projection(fovy, aspect, znear, zfar);
		break;
	}
	case rendermode::shadows:
	{
		// calculate lightview boundaries based on relevant area of the world camera frustum:
		// ...setup chunk of frustum we're interested in...
		auto const zfar = std::min(1.f, Global.shadowtune.depth / (Global.BaseDrawRange * Global.fDistanceFactor) * std::max(1.f, Global.ZoomFactor * 0.5f));
		renderpass_config worldview;
		setup_pass(Viewport, worldview, rendermode::color, 0.f, zfar, true);
		auto &frustumchunkshapepoints = worldview.pass_camera.frustum_points();
		// ...modelview matrix: determine the centre of frustum chunk in world space...
		glm::vec3 frustumchunkmin, frustumchunkmax;
		bounding_box(frustumchunkmin, frustumchunkmax, std::begin(frustumchunkshapepoints), std::end(frustumchunkshapepoints));
		auto const frustumchunkcentre = (frustumchunkmin + frustumchunkmax) * 0.5f;
		// ...cap the vertical angle to keep shadows from getting too long...
		auto const lightvector = glm::normalize(glm::vec3{m_sunlight.direction.x, std::min(m_sunlight.direction.y, -0.2f), m_sunlight.direction.z});
		// ...place the light source at the calculated centre and setup world space light view matrix...
		camera.position() = worldview.pass_camera.position() + glm::dvec3{frustumchunkcentre};
		viewmatrix *= glm::lookAt(camera.position(), camera.position() + glm::dvec3{lightvector}, glm::dvec3{0.f, 1.f, 0.f});
		// ...projection matrix: calculate boundaries of the frustum chunk in light space...
		auto const lightviewmatrix = glm::translate(glm::mat4{glm::mat3{viewmatrix}}, -frustumchunkcentre);
		for (auto &point : frustumchunkshapepoints)
		{
			point = lightviewmatrix * point;
		}
		bounding_box(frustumchunkmin, frustumchunkmax, std::begin(frustumchunkshapepoints), std::end(frustumchunkshapepoints));
		// quantize the frustum points and add some padding, to reduce shadow shimmer on scale changes
		auto const quantizationstep{std::min(Global.shadowtune.depth, 50.f)};
		frustumchunkmin = quantizationstep * glm::floor(frustumchunkmin * (1.f / quantizationstep));
		frustumchunkmax = quantizationstep * glm::ceil(frustumchunkmax * (1.f / quantizationstep));
		// ...use the dimensions to set up light projection boundaries...
		// NOTE: since we only have one cascade map stage, we extend the chunk forward/back to catch areas normally covered by other stages
		camera.projection() = ortho_projection(frustumchunkmin.x, frustumchunkmax.x, frustumchunkmin.y, frustumchunkmax.y, frustumchunkmin.z - 500.f, frustumchunkmax.z + 500.f);
		frustumtest_proj = ortho_frustumtest_projection(frustumchunkmin.x, frustumchunkmax.x, frustumchunkmin.y, frustumchunkmax.y, frustumchunkmin.z - 500.f, frustumchunkmax.z + 500.f);
		/*
		        // fixed ortho projection from old build, for quick quality comparisons
		        camera.projection() *=
					ortho_projection(
		                -Global.shadowtune.width, Global.shadowtune.width,
		                -Global.shadowtune.width, Global.shadowtune.width,
		                -Global.shadowtune.depth, Global.shadowtune.depth );
				camera.position() = Global.pCamera.Pos - glm::dvec3{ m_sunlight.direction };
				if( camera.position().y - Global.pCamera.Pos.y < 0.1 ) {
					camera.position().y = Global.pCamera.Pos.y + 0.1;
		        }
		        viewmatrix *= glm::lookAt(
		            camera.position(),
					glm::dvec3{ Global.pCamera.Pos },
		            glm::dvec3{ 0.f, 1.f, 0.f } );
		*/
		// ... and adjust the projection to sample complete shadow map texels:
		// get coordinates for a sample texel...
		auto shadowmaptexel = glm::vec2{camera.projection() * glm::mat4{viewmatrix} * glm::vec4{0.f, 0.f, 0.f, 1.f}};
		// ...convert result from clip space to texture coordinates, and calculate adjustment...
		shadowmaptexel *= m_shadowbuffersize * 0.5f;
		auto shadowmapadjustment = glm::round(shadowmaptexel) - shadowmaptexel;
		// ...transform coordinate change back to homogenous light space...
		shadowmapadjustment /= m_shadowbuffersize * 0.5f;
		// ... and bake the adjustment into the projection matrix
		camera.projection() = glm::translate(glm::mat4{1.f}, glm::vec3{shadowmapadjustment, 0.f}) * camera.projection();

		break;
	}
	case rendermode::cabshadows:
	{
		// fixed size cube large enough to enclose a vehicle compartment
		// modelview
		auto const lightvector = glm::normalize(glm::vec3{m_sunlight.direction.x, std::min(m_sunlight.direction.y, -0.2f), m_sunlight.direction.z});
		camera.position() = Global.pCamera.Pos - glm::dvec3{lightvector};
		viewmatrix *= glm::lookAt(camera.position(), glm::dvec3{Global.pCamera.Pos}, glm::dvec3{0.f, 1.f, 0.f});
		// projection
		auto const maphalfsize{Config.draw_range * 0.5f};
		camera.projection() = ortho_projection(-maphalfsize, maphalfsize, -maphalfsize, maphalfsize, -Config.draw_range, Config.draw_range);
		frustumtest_proj = ortho_frustumtest_projection(-maphalfsize, maphalfsize, -maphalfsize, maphalfsize, -Config.draw_range, Config.draw_range);

		/*
				// adjust the projection to sample complete shadow map texels
				auto shadowmaptexel = glm::vec2 { camera.projection() * glm::mat4{ viewmatrix } * glm::vec4{ 0.f, 0.f, 0.f, 1.f } };
				shadowmaptexel *= ( m_shadowbuffersize / 2 ) * 0.5f;
				auto shadowmapadjustment = glm::round( shadowmaptexel ) - shadowmaptexel;
				shadowmapadjustment /= ( m_shadowbuffersize / 2 ) * 0.5f;
				camera.projection() = glm::translate( glm::mat4{ 1.f }, glm::vec3{ shadowmapadjustment, 0.f } ) * camera.projection();
		*/
		break;
	}
	case rendermode::pickcontrols:
	case rendermode::pickscenery:
	{
		viewmatrix = glm::dmat4(Viewport.camera_transform);

		// modelview
		camera.position() = Global.pCamera.Pos;
		Global.pCamera.SetMatrix(viewmatrix);

		// projection
		float znear = 0.1f * Global.ZoomFactor;
		float zfar = Config.draw_range * Global.fDistanceFactor;
		camera.projection() = perspective_projection(fovy, aspect, znear, zfar);
		frustumtest_proj = perpsective_frustumtest_projection(fovy, aspect, znear, zfar);
		break;
	}
	case rendermode::reflections:
	{
		// modelview
		camera.position() = (((true == DebugCameraFlag) && (false == Ignoredebug)) ? Global.pDebugCamera.Pos : Global.pCamera.Pos);
		glm::dvec3 const cubefacetargetvectors[6] = {{1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, -1.0}};
		glm::dvec3 const cubefaceupvectors[6] = {{0.0, -1.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, -1.0}, {0.0, -1.0, 0.0}, {0.0, -1.0, 0.0}};
		auto const cubefaceindex = m_environmentcubetextureface;
		viewmatrix *= glm::lookAt(camera.position(), camera.position() + cubefacetargetvectors[cubefaceindex], cubefaceupvectors[cubefaceindex]);
		// projection
		float znear = 0.1f * Global.ZoomFactor;
		float zfar = Config.draw_range * Global.fDistanceFactor;
		camera.projection() = perspective_projection(glm::radians(90.f), 1.f, znear, zfar);
		frustumtest_proj = perpsective_frustumtest_projection(glm::radians(90.f), 1.f, znear, zfar);
		break;
	}
	default:
	{
		break;
	}
	}
	camera.modelview() = viewmatrix;
	camera.update_frustum(frustumtest_proj);
}

void opengl_renderer::setup_matrices()
{
	::glMatrixMode(GL_PROJECTION);
	OpenGLMatrices.load_matrix(m_renderpass.pass_camera.projection());

	// trim modelview matrix just to rotation, since rendering is done in camera-centric world space
	::glMatrixMode(GL_MODELVIEW);
	OpenGLMatrices.load_matrix(glm::mat4(glm::mat3(m_renderpass.pass_camera.modelview())));
}

void opengl_renderer::setup_drawing(bool const Alpha)
{
	if (Alpha)
	{
		glEnable(GL_BLEND);
		glDepthMask(GL_FALSE);
		m_blendingenabled = true;
	}
	else
	{
		glDisable(GL_BLEND);
		glDepthMask(GL_TRUE);
		m_blendingenabled = false;
	}

	switch (m_renderpass.draw_mode)
	{
	case rendermode::color:
	case rendermode::reflections:
	{
		glCullFace(GL_BACK);
		break;
	}
	case rendermode::shadows:
	case rendermode::cabshadows:
	{
		glCullFace(GL_FRONT);
		break;
	}
	case rendermode::pickcontrols:
	case rendermode::pickscenery:
	{
		glCullFace(GL_BACK);
		break;
	}
	default:
	{
		break;
	}
	}
}

// configures shadow texture unit for specified shadow map and conersion matrix
void opengl_renderer::setup_shadow_map(opengl_texture *tex, renderpass_config conf)
{
	if (tex)
		tex->bind(gl::MAX_TEXTURES + 0);
	else
		opengl_texture::unbind(gl::MAX_TEXTURES + 0);

	if (tex)
	{
		glm::mat4 coordmove;

		if (GLAD_GL_ARB_clip_control || GLAD_GL_EXT_clip_control)
			// transform 1..-1 NDC xy coordinates to 1..0
			coordmove = glm::mat4( //
			    0.5, 0.0, 0.0, 0.0, //
			    0.0, 0.5, 0.0, 0.0, //
			    0.0, 0.0, 1.0, 0.0, //
			    0.5, 0.5, 0.0, 1.0 //
			);
		else
			// without clip_control we also need to transform z
			coordmove = glm::mat4( //
			    0.5, 0.0, 0.0, 0.0, //
			    0.0, 0.5, 0.0, 0.0, //
			    0.0, 0.0, 0.5, 0.0, //
			    0.5, 0.5, 0.5, 1.0 //
			);
		glm::mat4 depthproj = conf.pass_camera.projection();
		glm::mat4 depthcam = conf.pass_camera.modelview();
		glm::mat4 worldcam = m_renderpass.pass_camera.modelview();

		scene_ubs.lightview = coordmove * depthproj * depthcam * glm::inverse(worldcam);
		scene_ubo->update(scene_ubs);
	}
}

void opengl_renderer::setup_env_map(gl::cubemap *tex)
{
	if (tex)
	{
		tex->bind(GL_TEXTURE0 + gl::MAX_TEXTURES + 1);
		glActiveTexture(GL_TEXTURE0);
	}
	else
	{
		glActiveTexture(GL_TEXTURE0 + gl::MAX_TEXTURES + 1);
		glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
		glActiveTexture(GL_TEXTURE0);
	}
	opengl_texture::reset_unit_cache();
}

void opengl_renderer::setup_environment_light(TEnvironmentType const Environment)
{

	switch (Environment)
	{
	case e_flat:
	{
		m_sunlight.apply_intensity();
		//            m_environment = Environment;
		break;
	}
	case e_canyon:
	{
		m_sunlight.apply_intensity(0.4f);
		//            m_environment = Environment;
		break;
	}
	case e_tunnel:
	{
		m_sunlight.apply_intensity(0.2f);
		//            m_environment = Environment;
		break;
	}
	default:
	{
		break;
	}
	}
}

bool opengl_renderer::Render(world_environment *Environment)
{

	// calculate shadow tone, based on positions of celestial bodies
	m_shadowcolor = interpolate(glm::vec4{colors::shadow}, glm::vec4{colors::white}, clamp(-Environment->m_sun.getAngle(), 0.f, 6.f) / 6.f);
	if ((Environment->m_sun.getAngle() < -18.f) && (Environment->m_moon.getAngle() > 0.f))
	{
		// turn on moon shadows after nautical twilight, if the moon is actually up
		m_shadowcolor = colors::shadow;
	}
	// soften shadows depending on sky overcast factor
	m_shadowcolor = glm::min(colors::white, m_shadowcolor + ((colors::white - colors::shadow) * Global.Overcast));

	if (Global.bWireFrame)
	{
		// bez nieba w trybie rysowania linii
		return false;
	}

	Bind_Material(null_handle);
	::glDisable(GL_DEPTH_TEST);
	::glPushMatrix();

	model_ubs.set_modelview(OpenGLMatrices.data(GL_MODELVIEW));
	model_ubo->update(model_ubs);

	// skydome
	// drawn with 500m radius to blend in if the fog range is low
	glPushMatrix();
	glScalef(500.0f, 500.0f, 500.0f);
	Environment->m_skydome.Render();
	glPopMatrix();

	// skydome uses a custom vbo which could potentially confuse the main geometry system. hardly elegant but, eh
	gfx::opengl_vbogeometrybank::reset();

	// stars
	if (Environment->m_stars.m_stars != nullptr)
	{
		// setup
		::glPushMatrix();
		::glRotatef(Environment->m_stars.m_latitude, 1.f, 0.f, 0.f); // ustawienie osi OY na północ
		::glRotatef(-std::fmod((float)Global.fTimeAngleDeg, 360.f), 0.f, 1.f, 0.f); // obrót dobowy osi OX

		// render
		GfxRenderer.Render(Environment->m_stars.m_stars, nullptr, 1.0);

		// post-render cleanup
		::glPopMatrix();
	}

	auto const fogfactor{clamp<float>(Global.fFogEnd / 2000.f, 0.f, 1.f)}; // stronger fog reduces opacity of the celestial bodies
	float const duskfactor = 1.0f - clamp(std::abs(Environment->m_sun.getAngle()), 0.0f, 12.0f) / 12.0f;
	glm::vec3 suncolor = interpolate(glm::vec3(255.0f / 255.0f, 242.0f / 255.0f, 231.0f / 255.0f), glm::vec3(235.0f / 255.0f, 140.0f / 255.0f, 36.0f / 255.0f), duskfactor);

	// clouds
	if (Environment->m_clouds.mdCloud)
	{
		// setup
		glm::vec3 color = interpolate(Environment->m_skydome.GetAverageColor(), suncolor, duskfactor * 0.25f) * interpolate(1.f, 0.35f, Global.Overcast / 2.f) // overcast darkens the clouds
		                  * 0.5f;

		// write cloud color into material
		TSubModel *mdl = Environment->m_clouds.mdCloud->Root;
		if (mdl->m_material != null_handle)
			m_materials.material(mdl->m_material).params[0] = glm::vec4(color, 1.0f);

		// render
		Render(Environment->m_clouds.mdCloud, nullptr, 100.0);
		Render_Alpha(Environment->m_clouds.mdCloud, nullptr, 100.0);
		// post-render cleanup
	}

	// celestial bodies

	m_celestial_shader->bind();
	m_empty_vao->bind();

	auto const &modelview = OpenGLMatrices.data(GL_MODELVIEW);

	// sun
	{
		Bind_Texture(0, m_suntexture);
		glm::vec4 color(suncolor.x, suncolor.y, suncolor.z, clamp(1.5f - Global.Overcast, 0.f, 1.f) * fogfactor);
		auto const sunvector = Environment->m_sun.getDirection();

		model_ubs.param[0] = color;
		model_ubs.param[1] = glm::vec4(glm::vec3(modelview * glm::vec4(sunvector, 1.0f)), 0.00463f);
		model_ubs.param[2] = glm::vec4(0.0f, 1.0f, 1.0f, 0.0f);
		model_ubo->update(model_ubs);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	// moon
	{
		Bind_Texture(0, m_moontexture);
		glm::vec3 mooncolor(255.0f / 255.0f, 242.0f / 255.0f, 231.0f / 255.0f);
		glm::vec4 color(mooncolor.r, mooncolor.g, mooncolor.b,
		                // fade the moon if it's near the sun in the sky, especially during the day
		                std::max<float>(0.f, 1.0 - 0.5 * Global.fLuminance - 0.65 * std::max(0.f, glm::dot(Environment->m_sun.getDirection(), Environment->m_moon.getDirection()))) * fogfactor);

		auto const moonvector = Environment->m_moon.getDirection();

		// choose the moon appearance variant, based on current moon phase
		// NOTE: implementation specific, 8 variants are laid out in 3x3 arrangement
		// from new moon onwards, top left to right bottom (last spot is left for future use, if any)
		auto const moonphase = Environment->m_moon.getPhase();
		float moonu, moonv;
		if (moonphase < 1.84566f)
		{
			moonv = 1.0f - 0.0f;
			moonu = 0.0f;
		}
		else if (moonphase < 5.53699f)
		{
			moonv = 1.0f - 0.0f;
			moonu = 0.333f;
		}
		else if (moonphase < 9.22831f)
		{
			moonv = 1.0f - 0.0f;
			moonu = 0.667f;
		}
		else if (moonphase < 12.91963f)
		{
			moonv = 1.0f - 0.333f;
			moonu = 0.0f;
		}
		else if (moonphase < 16.61096f)
		{
			moonv = 1.0f - 0.333f;
			moonu = 0.333f;
		}
		else if (moonphase < 20.30228f)
		{
			moonv = 1.0f - 0.333f;
			moonu = 0.667f;
		}
		else if (moonphase < 23.99361f)
		{
			moonv = 1.0f - 0.667f;
			moonu = 0.0f;
		}
		else if (moonphase < 27.68493f)
		{
			moonv = 1.0f - 0.667f;
			moonu = 0.333f;
		}
		else
		{
			moonv = 1.0f - 0.0f;
			moonu = 0.0f;
		}

		model_ubs.param[0] = color;
		model_ubs.param[1] = glm::vec4(glm::vec3(modelview * glm::vec4(moonvector, 1.0f)), 0.00451f);
		model_ubs.param[2] = glm::vec4(moonu, moonv, 0.333f, 0.0f);
		model_ubo->update(model_ubs);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}

	gl::program::unbind();
	gl::vao::unbind();
	::glPopMatrix();
	::glEnable(GL_DEPTH_TEST);

	m_sunlight.apply_angle();
	m_sunlight.apply_intensity();

	return true;
}

// geometry methods
// creates a new geometry bank. returns: handle to the bank or NULL
gfx::geometrybank_handle opengl_renderer::Create_Bank()
{

	return m_geometry.create_bank();
}

// creates a new geometry chunk of specified type from supplied vertex data, in specified bank. returns: handle to the chunk or NULL
gfx::geometry_handle opengl_renderer::Insert(gfx::vertex_array &Vertices, gfx::geometrybank_handle const &Geometry, int const Type)
{
	gfx::calculate_tangent(Vertices, Type);

	return m_geometry.create_chunk(Vertices, Geometry, Type);
}

// replaces data of specified chunk with the supplied vertex data, starting from specified offset
bool opengl_renderer::Replace(gfx::vertex_array &Vertices, gfx::geometry_handle const &Geometry, int const Type, std::size_t const Offset)
{
	gfx::calculate_tangent(Vertices, Type);

	return m_geometry.replace(Vertices, Geometry, Offset);
}

// adds supplied vertex data at the end of specified chunk
bool opengl_renderer::Append(gfx::vertex_array &Vertices, gfx::geometry_handle const &Geometry, int const Type)
{
	gfx::calculate_tangent(Vertices, Type);

	return m_geometry.append(Vertices, Geometry);
}

// provides direct access to vertex data of specfied chunk
gfx::vertex_array const &opengl_renderer::Vertices(gfx::geometry_handle const &Geometry) const
{

	return m_geometry.vertices(Geometry);
}

// material methods
material_handle opengl_renderer::Fetch_Material(std::string const &Filename, bool const Loadnow)
{

	return m_materials.create(Filename, Loadnow);
}

std::shared_ptr<gl::program> opengl_renderer::Fetch_Shader(const std::string &name)
{
	auto it = m_shaders.find(name);
	if (it == m_shaders.end())
	{
		gl::shader fragment("mat_" + name + ".frag");
		gl::program *program = new gl::program({fragment, *m_vertex_shader.get()});
		m_shaders.insert({name, std::shared_ptr<gl::program>(program)});
	}

	return m_shaders[name];
}

void opengl_renderer::Bind_Material(material_handle const Material, TSubModel *sm)
{
	if (Material != null_handle)
	{
		auto &material = m_materials.material(Material);

		memcpy(&model_ubs.param[0], &material.params[0], sizeof(model_ubs.param));

		for (size_t i = 0; i < material.params_state.size(); i++)
		{
			gl::shader::param_entry entry = material.params_state[i];

			glm::vec4 src(1.0f);

			if (sm)
			{
				if (entry.defaultparam == gl::shader::defaultparam_e::ambient)
					src = sm->f4Ambient;
				else if (entry.defaultparam == gl::shader::defaultparam_e::diffuse)
					src = sm->f4Diffuse;
				else if (entry.defaultparam == gl::shader::defaultparam_e::specular)
					src = sm->f4Specular;
			}

			for (size_t j = 0; j < entry.size; j++)
				model_ubs.param[entry.location][entry.offset + j] = src[j];
		}

		if (m_blendingenabled)
		{
			model_ubs.opacity = -1.0f;
		}
		else
		{
			if (!std::isnan(material.opacity))
				model_ubs.opacity = material.opacity;
			else
				model_ubs.opacity = 0.5f;
		}

		if (sm)
			model_ubs.alpha_mult = sm->fVisible;
		else
			model_ubs.alpha_mult = 1.0f;

		if (GLAD_GL_ARB_multi_bind)
		{
			GLuint lastdiff = 0;
			size_t i;
			for (i = 0; i < gl::MAX_TEXTURES; i++)
				if (material.textures[i] != null_handle)
				{
					opengl_texture &tex = m_textures.mark_as_used(material.textures[i]);
					tex.create();
					if (opengl_texture::units[i] != tex.id)
					{
						opengl_texture::units[i] = tex.id;
						lastdiff = i + 1;
					}
				}
				else
					break;

			if (lastdiff)
				glBindTextures(0, lastdiff, &opengl_texture::units[0]);
		}
		else
		{
			size_t unit = 0;
			for (auto &tex : material.textures)
			{
				if (tex == null_handle)
					break;
				m_textures.bind(unit, tex);
				unit++;
			}
		}

		material.shader->bind();
	}
	else if (Material != m_invalid_material)
		Bind_Material(m_invalid_material);
}

void opengl_renderer::Bind_Material_Shadow(material_handle const Material)
{
	if (Material != null_handle)
	{
		auto &material = m_materials.material(Material);

		if (material.textures[0] != null_handle)
		{
			m_textures.bind(0, material.textures[0]);
			m_alpha_shadow_shader->bind();
		}
		else
			m_shadow_shader->bind();
	}
	else
		m_shadow_shader->bind();
}

opengl_material const &opengl_renderer::Material(material_handle const Material) const
{
	return m_materials.material(Material);
}

opengl_material &opengl_renderer::Material(material_handle const Material)
{
	return m_materials.material(Material);
}

texture_handle opengl_renderer::Fetch_Texture(std::string const &Filename, bool const Loadnow, GLint format_hint)
{
	return m_textures.create(Filename, Loadnow, format_hint);
}

void opengl_renderer::Bind_Texture(size_t Unit, texture_handle const Texture)
{
	m_textures.bind(Unit, Texture);
}

opengl_texture &opengl_renderer::Texture(texture_handle const Texture) const
{

	return m_textures.texture(Texture);
}

void opengl_renderer::Update_AnimModel(TAnimModel *model)
{
	model->RaAnimate(m_framestamp);
}

void opengl_renderer::Render(scene::basic_region *Region)
{

	m_sectionqueue.clear();
	m_cellqueue.clear();
	// build a list of region sections to render
	glm::vec3 const cameraposition{m_renderpass.pass_camera.position()};
	auto const camerax = static_cast<int>(std::floor(cameraposition.x / scene::EU07_SECTIONSIZE + scene::EU07_REGIONSIDESECTIONCOUNT / 2));
	auto const cameraz = static_cast<int>(std::floor(cameraposition.z / scene::EU07_SECTIONSIZE + scene::EU07_REGIONSIDESECTIONCOUNT / 2));
	int const segmentcount = 2 * static_cast<int>(std::ceil(m_renderpass.draw_range * Global.fDistanceFactor / scene::EU07_SECTIONSIZE));
	int const originx = camerax - segmentcount / 2;
	int const originz = cameraz - segmentcount / 2;

	for (int row = originz; row <= originz + segmentcount; ++row)
	{
		if (row < 0)
		{
			continue;
		}
		if (row >= scene::EU07_REGIONSIDESECTIONCOUNT)
		{
			break;
		}
		for (int column = originx; column <= originx + segmentcount; ++column)
		{
			if (column < 0)
			{
				continue;
			}
			if (column >= scene::EU07_REGIONSIDESECTIONCOUNT)
			{
				break;
			}
			auto *section{Region->m_sections[row * scene::EU07_REGIONSIDESECTIONCOUNT + column]};
			if ((section != nullptr) && (m_renderpass.pass_camera.visible(section->m_area)))
			{
				m_sectionqueue.emplace_back(section);
			}
		}
	}

	switch (m_renderpass.draw_mode)
	{
	case rendermode::color:
	{
		Update_Lights(simulation::Lights);

		Render(std::begin(m_sectionqueue), std::end(m_sectionqueue));
		// draw queue is filled while rendering sections
		if (EditorModeFlag && m_current_viewport->main)
		{
			// when editor mode is active calculate world position of the cursor
			// at this stage the z-buffer is filled with only ground geometry
			get_mouse_depth();
		}
		Render(std::begin(m_cellqueue), std::end(m_cellqueue));
		break;
	}
	case rendermode::shadows:
	case rendermode::pickscenery:
	{
		// these render modes don't bother with lights
		Render(std::begin(m_sectionqueue), std::end(m_sectionqueue));
		// they can also skip queue sorting, as they only deal with opaque geometry
		// NOTE: there's benefit from rendering front-to-back, but is it significant enough? TODO: investigate
		Render(std::begin(m_cellqueue), std::end(m_cellqueue));
		break;
	}
	case rendermode::reflections:
	{
		// for the time being reflections render only terrain geometry
		Render(std::begin(m_sectionqueue), std::end(m_sectionqueue));
		break;
	}
	case rendermode::pickcontrols:
	default:
	{
		// no need to render anything ourside of the cab in control picking mode
		break;
	}
	}
}

void opengl_renderer::Render(section_sequence::iterator First, section_sequence::iterator Last)
{

	switch (m_renderpass.draw_mode)
	{
	case rendermode::color:
	case rendermode::reflections:
	case rendermode::shadows:
		break;
	case rendermode::pickscenery:
	{
		// non-interactive scenery elements get neutral colour
		model_ubs.param[0] = colors::none;
		break;
	}
	default:
		break;
	}

	while (First != Last)
	{

		auto *section = *First;
		section->create_geometry();

		// render shapes held by the section
		switch (m_renderpass.draw_mode)
		{
		case rendermode::color:
		case rendermode::reflections:
		case rendermode::shadows:
		case rendermode::pickscenery:
		{
			if (false == section->m_shapes.empty())
			{
				// since all shapes of the section share center point we can optimize out a few calls here
				::glPushMatrix();
				auto const originoffset{section->m_area.center - m_renderpass.pass_camera.position()};
				::glTranslated(originoffset.x, originoffset.y, originoffset.z);
				// render
				for (auto const &shape : section->m_shapes)
				{
					Render(shape, true);
				}
				// post-render cleanup
				::glPopMatrix();
			}
			break;
		}
		case rendermode::pickcontrols:
		default:
		{
			break;
		}
		}

		// add the section's cells to the cell queue
		switch (m_renderpass.draw_mode)
		{
		case rendermode::color:
		case rendermode::shadows:
		case rendermode::pickscenery:
		{
			for (auto &cell : section->m_cells)
			{
				if ((true == cell.m_active) && (m_renderpass.pass_camera.visible(cell.m_area)))
				{
					// store visible cells with content as well as their current distance, for sorting later
					m_cellqueue.emplace_back(glm::length2(m_renderpass.pass_camera.position() - cell.m_area.center), &cell);
				}
			}
			break;
		}
		case rendermode::reflections:
		case rendermode::pickcontrols:
		default:
		{
			break;
		}
		}
		// proceed to next section
		++First;
	}

	switch (m_renderpass.draw_mode)
	{
	case rendermode::shadows:
	{
		break;
	}
	default:
	{
		break;
	}
	}
}

void opengl_renderer::Render(cell_sequence::iterator First, cell_sequence::iterator Last)
{

	// cache initial iterator for the second sweep
	auto first{First};
	// first pass draws elements which we know are located in section banks, to reduce vbo switching
	while (First != Last)
	{

		auto *cell = First->second;
		// przeliczenia animacji torów w sektorze
		cell->RaAnimate(m_framestamp);

		switch (m_renderpass.draw_mode)
		{
		case rendermode::color:
		{
			// since all shapes of the section share center point we can optimize out a few calls here
			::glPushMatrix();
			auto const originoffset{cell->m_area.center - m_renderpass.pass_camera.position()};
			::glTranslated(originoffset.x, originoffset.y, originoffset.z);

			// render
			// opaque non-instanced shapes
			for (auto const &shape : cell->m_shapesopaque)
			{
				Render(shape, false);
			}
			// tracks
			// TODO: update after path node refactoring
			Render(std::begin(cell->m_paths), std::end(cell->m_paths));
			// post-render cleanup
			::glPopMatrix();

			break;
		}
		case rendermode::shadows:
		{
			// since all shapes of the section share center point we can optimize out a few calls here
			::glPushMatrix();
			auto const originoffset{cell->m_area.center - m_renderpass.pass_camera.position()};
			::glTranslated(originoffset.x, originoffset.y, originoffset.z);

			// render
			// opaque non-instanced shapes
			for (auto const &shape : cell->m_shapesopaque)
				Render(shape, false);
			// tracks
			Render(std::begin(cell->m_paths), std::end(cell->m_paths));

			// post-render cleanup
			::glPopMatrix();

			break;
		}
		case rendermode::pickscenery:
		{
			// same procedure like with regular render, but editor-enabled nodes receive custom colour used for picking
			// since all shapes of the section share center point we can optimize out a few calls here
			::glPushMatrix();
			auto const originoffset{cell->m_area.center - m_renderpass.pass_camera.position()};
			::glTranslated(originoffset.x, originoffset.y, originoffset.z);
			// render
			// opaque non-instanced shapes
			// non-interactive scenery elements get neutral colour
			model_ubs.param[0] = colors::none;
			for (auto const &shape : cell->m_shapesopaque)
				Render(shape, false);
			// tracks
			for (auto *path : cell->m_paths)
			{
				model_ubs.param[0] = glm::vec4(pick_color(m_picksceneryitems.size() + 1), 1.0f);
				Render(path);
			}
			// post-render cleanup
			::glPopMatrix();
			break;
		}
		case rendermode::reflections:
		case rendermode::pickcontrols:
		default:
		{
			break;
		}
		}

		++First;
	}
	// second pass draws elements with their own vbos
	while (first != Last)
	{

		auto const *cell = first->second;

		switch (m_renderpass.draw_mode)
		{
		case rendermode::color:
		case rendermode::shadows:
		{
			// opaque parts of instanced models
			for (auto *instance : cell->m_instancesopaque)
			{
				Render(instance);
			}
			// opaque parts of vehicles
			for (auto *path : cell->m_paths)
			{
				for (auto *dynamic : path->Dynamics)
				{
					Render(dynamic);
				}
			}
			break;
		}
		case rendermode::pickscenery:
		{
			// opaque parts of instanced models
			// same procedure like with regular render, but each node receives custom colour used for picking
			for (auto *instance : cell->m_instancesopaque)
			{
				model_ubs.param[0] = glm::vec4(pick_color(m_picksceneryitems.size() + 1), 1.0f);
				Render(instance);
			}
			// vehicles aren't included in scenery picking for the time being
			break;
		}
		case rendermode::reflections:
		case rendermode::pickcontrols:
		default:
		{
			break;
		}
		}

		++first;
	}
}

void opengl_renderer::Draw_Geometry(std::vector<gfx::geometrybank_handle>::iterator begin, std::vector<gfx::geometrybank_handle>::iterator end)
{
	m_geometry.draw(begin, end);
}

void opengl_renderer::Draw_Geometry(const gfx::geometrybank_handle &handle)
{
	m_geometry.draw(handle);
}

void opengl_renderer::draw(const gfx::geometry_handle &handle)
{
	model_ubs.set_modelview(OpenGLMatrices.data(GL_MODELVIEW));
	model_ubo->update(model_ubs);

	m_geometry.draw(handle);
}

void opengl_renderer::draw(std::vector<gfx::geometrybank_handle>::iterator it, std::vector<gfx::geometrybank_handle>::iterator end)
{
	model_ubs.set_modelview(OpenGLMatrices.data(GL_MODELVIEW));
	model_ubo->update(model_ubs);

	Draw_Geometry(it, end);
}

void opengl_renderer::Render(scene::shape_node const &Shape, bool const Ignorerange)
{
	auto const &data{Shape.data()};

	if (false == Ignorerange)
	{
		double distancesquared;
		switch (m_renderpass.draw_mode)
		{
		case rendermode::shadows:
		{
			// 'camera' for the light pass is the light source, but we need to draw what the 'real' camera sees
			distancesquared = Math3D::SquareMagnitude((data.area.center - m_renderpass.viewport_camera.position()) / (double)Global.ZoomFactor) / Global.fDistanceFactor;
			break;
		}
		default:
		{
			distancesquared = glm::length2((data.area.center - m_renderpass.pass_camera.position()) / (double)Global.ZoomFactor) / Global.fDistanceFactor;
			break;
		}
		}
		if ((distancesquared < data.rangesquared_min) || (distancesquared >= data.rangesquared_max))
		{
			return;
		}
	}

	// setup
	switch (m_renderpass.draw_mode)
	{
	case rendermode::color:
	case rendermode::reflections:
		Bind_Material(data.material);
		break;
	case rendermode::shadows:
		Bind_Material_Shadow(data.material);
		break;
	case rendermode::pickscenery:
	case rendermode::pickcontrols:
		m_pick_shader->bind();
		break;
	default:
		break;
	}
	// render

	draw(data.geometry);
	// debug data
	++m_debugstats.shapes;
	++m_debugstats.drawcalls;
}

void opengl_renderer::Render(TAnimModel *Instance)
{

	if (false == Instance->m_visible)
	{
		return;
	}

	double distancesquared;
	switch (m_renderpass.draw_mode)
	{
	case rendermode::shadows:
	{
		// 'camera' for the light pass is the light source, but we need to draw what the 'real' camera sees
		distancesquared = Math3D::SquareMagnitude((Instance->location() - m_renderpass.viewport_camera.position()) / (double)Global.ZoomFactor) / Global.fDistanceFactor;
		break;
	}
	default:
	{
		distancesquared = Math3D::SquareMagnitude((Instance->location() - m_renderpass.pass_camera.position()) / (double)Global.ZoomFactor) / Global.fDistanceFactor;
		break;
	}
	}
	if ((distancesquared < Instance->m_rangesquaredmin) || (distancesquared >= Instance->m_rangesquaredmax))
	{
		return;
	}

	switch (m_renderpass.draw_mode)
	{
	case rendermode::pickscenery:
	{
		// add the node to the pick list
		m_picksceneryitems.emplace_back(Instance);
		break;
	}
	default:
	{
		break;
	}
	}

	Instance->RaAnimate(m_framestamp); // jednorazowe przeliczenie animacji
	Instance->RaPrepare();
	if (Instance->pModel)
	{
		// renderowanie rekurencyjne submodeli
		Render(Instance->pModel, Instance->Material(), distancesquared, Instance->location() - m_renderpass.pass_camera.position(), Instance->vAngle);
	}
}

bool opengl_renderer::Render(TDynamicObject *Dynamic)
{
	glDebug("Render TDynamicObject");

	Dynamic->renderme = m_renderpass.pass_camera.visible(Dynamic);
	if (false == Dynamic->renderme)
	{
		return false;
	}
	// debug data
	++m_debugstats.dynamics;

	// setup
	TSubModel::iInstance = reinterpret_cast<std::uintptr_t>(Dynamic); //żeby nie robić cudzych animacji
	glm::dvec3 const originoffset = Dynamic->vPosition - m_renderpass.pass_camera.position();
	// lod visibility ranges are defined for base (x 1.0) viewing distance. for render we adjust them for actual range multiplier and zoom
	float squaredistance;
	switch (m_renderpass.draw_mode)
	{
	case rendermode::shadows:
	{
		squaredistance = glm::length2(glm::vec3{glm::dvec3{Dynamic->vPosition - m_renderpass.viewport_camera.position()}} / Global.ZoomFactor) / Global.fDistanceFactor;
		break;
	}
	default:
	{
		squaredistance = glm::length2(glm::vec3{originoffset} / Global.ZoomFactor) / Global.fDistanceFactor;
		break;
	}
	}
	Dynamic->ABuLittleUpdate(squaredistance); // ustawianie zmiennych submodeli dla wspólnego modelu

	glm::mat4 future_stack = model_ubs.future;

	glm::mat4 mv = OpenGLMatrices.data(GL_MODELVIEW);
	model_ubs.future *= glm::translate(mv, glm::vec3(Dynamic->get_future_movement())) * glm::inverse(mv);

	::glPushMatrix();
	::glTranslated(originoffset.x, originoffset.y, originoffset.z);
	::glMultMatrixd(Dynamic->mMatrix.getArray());

	switch (m_renderpass.draw_mode)
	{

	case rendermode::color:
	{
		if (Dynamic->fShade > 0.0f)
		{
			// change light level based on light level of the occupied track
			m_sunlight.apply_intensity(Dynamic->fShade);
		}

		// render
		if (Dynamic->mdLowPolyInt)
			Render(Dynamic->mdLowPolyInt, Dynamic->Material(), squaredistance);

		if (Dynamic->mdModel)
			Render(Dynamic->mdModel, Dynamic->Material(), squaredistance);

		if (Dynamic->mdLoad) // renderowanie nieprzezroczystego ładunku
			Render(Dynamic->mdLoad, Dynamic->Material(), squaredistance, {0.f, Dynamic->LoadOffset, 0.f}, {});

		// post-render cleanup

		if (Dynamic->fShade > 0.0f)
		{
			// restore regular light level
			m_sunlight.apply_intensity();
		}
		break;
	}
	case rendermode::shadows:
	{
		if (Dynamic->mdLowPolyInt)
		{
			// low poly interior
			Render(Dynamic->mdLowPolyInt, Dynamic->Material(), squaredistance);
		}
		if (Dynamic->mdModel)
			Render(Dynamic->mdModel, Dynamic->Material(), squaredistance);
		if (Dynamic->mdLoad) // renderowanie nieprzezroczystego ładunku
			Render(Dynamic->mdLoad, Dynamic->Material(), squaredistance, {0.f, Dynamic->LoadOffset, 0.f}, {});
		// post-render cleanup
		break;
	}
	case rendermode::pickcontrols:
	{
		if (Dynamic->mdLowPolyInt) {
			// low poly interior
			Render(Dynamic->mdLowPolyInt, Dynamic->Material(), squaredistance);
		}
		break;
	}
	case rendermode::pickscenery:
	default:
	{
		break;
	}
	}

	::glPopMatrix();

	model_ubs.future = future_stack;

	// TODO: check if this reset is needed. In theory each object should render all parts based on its own instance data anyway?
	if (Dynamic->btnOn)
		Dynamic->TurnOff(); // przywrócenie domyślnych pozycji submodeli

	return true;
}

// rendering kabiny gdy jest oddzielnym modelem i ma byc wyswietlana
bool opengl_renderer::Render_cab(TDynamicObject const *Dynamic, float const Lightlevel, bool const Alpha)
{

	if (Dynamic == nullptr)
	{

		TSubModel::iInstance = 0;
		return false;
	}

	TSubModel::iInstance = reinterpret_cast<std::uintptr_t>(Dynamic);

	if ((true == FreeFlyModeFlag) || (false == Dynamic->bDisplayCab) || (Dynamic->mdKabina == Dynamic->mdModel))
	{
		// ABu: Rendering kabiny jako ostatniej, zeby bylo widac przez szyby, tylko w widoku ze srodka
		return false;
	}

	if (Dynamic->mdKabina)
	{ // bo mogła zniknąć przy przechodzeniu do innego pojazdu
		// setup shared by all render paths
		::glPushMatrix();

		auto const originoffset = Dynamic->GetPosition() - m_renderpass.pass_camera.position();
		::glTranslated(originoffset.x, originoffset.y, originoffset.z);
		::glMultMatrixd(Dynamic->mMatrix.readArray());

		switch (m_renderpass.draw_mode)
		{
		case rendermode::color:
		{
			// render path specific setup:
			if (Dynamic->fShade > 0.0f)
			{
				// change light level based on light level of the occupied track
				m_sunlight.apply_intensity(Dynamic->fShade);
			}

			// crude way to light the cabin, until we have something more complete in place
			glm::vec3 old_ambient = light_ubs.ambient;
			light_ubs.ambient += Dynamic->InteriorLight * Lightlevel;
			light_ubo->update(light_ubs);

			// render
			if (true == Alpha)
			{
				// translucent parts
				Render_Alpha(Dynamic->mdKabina, Dynamic->Material(), 0.0);
			}
			else
			{
				// opaque parts
				Render(Dynamic->mdKabina, Dynamic->Material(), 0.0);
			}
			// post-render restore
			if (Dynamic->fShade > 0.0f)
			{
				// change light level based on light level of the occupied track
				m_sunlight.apply_intensity();
			}

			// restore ambient
			light_ubs.ambient = old_ambient;
			light_ubo->update(light_ubs);

			break;
		}
		case rendermode::cabshadows:
			if (true == Alpha)
				// translucent parts
				Render_Alpha(Dynamic->mdKabina, Dynamic->Material(), 0.0);
			else
				// opaque parts
				Render(Dynamic->mdKabina, Dynamic->Material(), 0.0);
			break;
		case rendermode::pickcontrols:
		{
			Render(Dynamic->mdKabina, Dynamic->Material(), 0.0);
			break;
		}
		default:
		{
			break;
		}
		}
		// post-render restore
		::glPopMatrix();
	}

	return true;
}

bool opengl_renderer::Render(TModel3d *Model, material_data const *Material, float const Squaredistance)
{

	auto alpha = (Material != nullptr ? Material->textures_alpha : 0x30300030);
	alpha ^= 0x0F0F000F; // odwrócenie flag tekstur, aby wyłapać nieprzezroczyste
	if (0 == (alpha & Model->iFlags & 0x1F1F001F))
	{
		// czy w ogóle jest co robić w tym cyklu?
		return false;
	}

	Model->Root->fSquareDist = Squaredistance; // zmienna globalna!

	// setup
	Model->Root->ReplacableSet((Material != nullptr ? Material->replacable_skins : nullptr), alpha);

	Model->Root->pRoot = Model;

	// render
	Render(Model->Root);

	// debug data
	++m_debugstats.models;

	// post-render cleanup

	return true;
}

bool opengl_renderer::Render(TModel3d *Model, material_data const *Material, float const Squaredistance, Math3D::vector3 const &Position, glm::vec3 const &A)
{
	Math3D::vector3 Angle(A);
	::glPushMatrix();
	::glTranslated(Position.x, Position.y, Position.z);
	if (Angle.y != 0.0)
		::glRotated(Angle.y, 0.0, 1.0, 0.0);
	if (Angle.x != 0.0)
		::glRotated(Angle.x, 1.0, 0.0, 0.0);
	if (Angle.z != 0.0)
		::glRotated(Angle.z, 0.0, 0.0, 1.0);

	auto const result = Render(Model, Material, Squaredistance);

	::glPopMatrix();

	return result;
}

void opengl_renderer::Render(TSubModel *Submodel)
{
	glDebug("Render TSubModel");

	if ((Submodel->iVisible) && (TSubModel::fSquareDist >= Submodel->fSquareMinDist) && (TSubModel::fSquareDist < Submodel->fSquareMaxDist))
	{

		// debug data
		++m_debugstats.submodels;
		++m_debugstats.drawcalls;

		glm::mat4 future_stack = model_ubs.future;

		if (Submodel->iFlags & 0xC000)
		{
			::glPushMatrix();
			if (Submodel->fMatrix)
				::glMultMatrixf(Submodel->fMatrix->readArray());
			if (Submodel->b_aAnim != TAnimType::at_None)
			{
				Submodel->RaAnimation(Submodel->b_aAnim);

				glm::mat4 mv = OpenGLMatrices.data(GL_MODELVIEW);
				model_ubs.future *= (mv * Submodel->future_transform) * glm::inverse(mv);
			}
		}

		if (Submodel->eType < TP_ROTATOR)
		{
			// renderowanie obiektów OpenGL
			if (Submodel->iAlpha & Submodel->iFlags & 0x1F)
			{
				// rysuj gdy element nieprzezroczysty
				switch (m_renderpass.draw_mode)
				{
				case rendermode::color:
				case rendermode::reflections:
				{
					// material configuration:
					// transparency hack
					if (Submodel->fVisible < 1.0f)
						setup_drawing(true);

					// textures...
					if (Submodel->m_material < 0)
					{ // zmienialne skóry
						Bind_Material(Submodel->ReplacableSkinId[-Submodel->m_material], Submodel);
					}
					else
					{
						// również 0
						Bind_Material(Submodel->m_material, Submodel);
					}

					// ...luminance
					auto const isemissive { ( Submodel->f4Emision.a > 0.f ) && ( Global.fLuminance < Submodel->fLight ) };
					if (isemissive)
						model_ubs.emission = Submodel->f4Emision.a;

					// main draw call
					draw(Submodel->m_geometry);

					// post-draw reset
					model_ubs.emission = 0.0f;
					if (Submodel->fVisible < 1.0f)
						setup_drawing(false);

					break;
				}
				case rendermode::shadows:
				case rendermode::cabshadows:
				{
					if (Submodel->m_material < 0)
					{ // zmienialne skóry
						Bind_Material_Shadow(Submodel->ReplacableSkinId[-Submodel->m_material]);
					}
					else
					{
						// również 0
						Bind_Material_Shadow(Submodel->m_material);
					}
					draw(Submodel->m_geometry);
					break;
				}
				case rendermode::pickscenery:
				{
					m_pick_shader->bind();
					draw(Submodel->m_geometry);
					break;
				}
				case rendermode::pickcontrols:
				{
					m_pick_shader->bind();
					// control picking applies individual colour for each submodel
					m_pickcontrolsitems.emplace_back(Submodel);
					model_ubs.param[0] = glm::vec4(pick_color(m_pickcontrolsitems.size()), 1.0f);
					draw(Submodel->m_geometry);
					break;
				}
				default:
				{
					break;
				}
				}
			}
		}
		else if (Submodel->eType == TP_FREESPOTLIGHT)
		{

			switch (m_renderpass.draw_mode)
			{
			// spotlights are only rendered in colour mode(s)
			case rendermode::color:
			case rendermode::reflections:
			{
				auto const &modelview = OpenGLMatrices.data(GL_MODELVIEW);
				auto const lightcenter = modelview * interpolate(glm::vec4(0.f, 0.f, -0.05f, 1.f), glm::vec4(0.f, 0.f, -0.25f, 1.f),
				                                                 static_cast<float>(TSubModel::fSquareDist / Submodel->fSquareMaxDist)); // pozycja punktu świecącego względem kamery
				Submodel->fCosViewAngle = glm::dot(glm::normalize(modelview * glm::vec4(0.f, 0.f, -1.f, 1.f) - lightcenter), glm::normalize(-lightcenter));

				if (Submodel->fCosViewAngle > Submodel->fCosFalloffAngle)
				{
					// kąt większy niż maksymalny stożek swiatła
					float lightlevel = 1.f; // TODO, TBD: parameter to control light strength
					// view angle attenuation
					float const anglefactor = clamp((Submodel->fCosViewAngle - Submodel->fCosFalloffAngle) / (Submodel->fCosHotspotAngle - Submodel->fCosFalloffAngle), 0.f, 1.f);
					lightlevel *= anglefactor;

					// distance attenuation. NOTE: since it's fixed pipeline with built-in gamma correction we're using linear attenuation
					// we're capping how much effect the distance attenuation can have, otherwise the lights get too tiny at regular distances
					float const distancefactor{std::max(0.5f, (Submodel->fSquareMaxDist - TSubModel::fSquareDist) / Submodel->fSquareMaxDist)};
					auto const pointsize{std::max(3.f, 5.f * distancefactor * anglefactor)};
					auto const resolutionratio { Global.iWindowHeight / 1080.f };
					// additionally reduce light strength for farther sources in rain or snow
					if (Global.Overcast > 0.75f)
					{
						float const precipitationfactor{interpolate(interpolate(1.f, 0.25f, clamp(Global.Overcast * 0.75f - 0.5f, 0.f, 1.f)), 1.f, distancefactor)};
						lightlevel *= precipitationfactor;
					}

					if (lightlevel > 0.f)
					{
						::glEnable(GL_BLEND);

						::glPushMatrix();
						::glLoadIdentity();
						::glTranslatef(lightcenter.x, lightcenter.y, lightcenter.z); // początek układu zostaje bez zmian

						// material configuration:
						// limit impact of dense fog on the lights
						auto const lightrange { std::max<float>( 500, m_fogrange * 2 ) }; // arbitrary, visibility at least 750m
						model_ubs.fog_density = 1.0 / lightrange;

						// main draw call
						model_ubs.emission = 1.0f;

						auto const lightcolor = glm::vec3(Submodel->DiffuseOverride.r < 0.f ? // -1 indicates no override
						                           Submodel->f4Diffuse :
						                           Submodel->DiffuseOverride);

						m_freespot_shader->bind();

						if (Global.Overcast > 1.0f)
						{
							// fake fog halo
							float const fogfactor{interpolate(2.f, 1.f, clamp<float>(Global.fFogEnd / 2000, 0.f, 1.f)) * std::max(1.f, Global.Overcast)};
							model_ubs.param[1].x = pointsize * resolutionratio * fogfactor * 2.0f;
							model_ubs.param[0] = glm::vec4(glm::vec3(lightcolor), Submodel->fVisible * std::min(1.f, lightlevel) * 0.5f);

							glDepthMask(GL_FALSE);
							draw(Submodel->m_geometry);
							glDepthMask(GL_TRUE);
						}
						model_ubs.param[1].x = pointsize * resolutionratio * 2.0f;
						model_ubs.param[0] = glm::vec4(glm::vec3(lightcolor), Submodel->fVisible * std::min(1.f, lightlevel));

						draw(Submodel->m_geometry);

						// post-draw reset
						model_ubs.emission = 0.0f;
						model_ubs.fog_density = 1.0f / m_fogrange;

						glDisable(GL_BLEND);

						::glPopMatrix();
					}
				}
				break;
			}
			default:
			{
				break;
			}
			}
		}
		else if (Submodel->eType == TP_STARS)
		{

			switch (m_renderpass.draw_mode)
			{
			// colour points are only rendered in colour mode(s)
			case rendermode::color:
			case rendermode::reflections:
			{
				if (Global.fLuminance < Submodel->fLight)
				{
					Bind_Material(Submodel->m_material, Submodel);

					// main draw call
					model_ubs.param[1].x = 2.0f * 2.0f;

					draw(Submodel->m_geometry);
				}
				break;
			}
			default:
			{
				break;
			}
			}
		}
		if (Submodel->Child != nullptr)
			if (Submodel->iAlpha & Submodel->iFlags & 0x001F0000)
				Render(Submodel->Child);

		if (Submodel->iFlags & 0xC000)
		{
			model_ubs.future = future_stack;
			::glPopMatrix();
		}
	}
	/*
	    if( Submodel->b_Anim < at_SecondsJump )
	        Submodel->b_Anim = at_None; // wyłączenie animacji dla kolejnego użycia subm
	*/
	if (Submodel->Next)
		if (Submodel->iAlpha & Submodel->iFlags & 0x1F000000)
			Render(Submodel->Next); // dalsze rekurencyjnie
}

void opengl_renderer::Render(TTrack *Track)
{
	if ((Track->m_material1 == 0) && (Track->m_material2 == 0) && (Track->eType != tt_Switch || Track->SwitchExtension->m_material3 == 0))
	{
		return;
	}
	if (false == Track->m_visible)
	{
		return;
	}

	++m_debugstats.paths;
	++m_debugstats.drawcalls;

	switch (m_renderpass.draw_mode)
	{
	// single path pieces are rendererd in pick scenery mode only
	case rendermode::pickscenery:
	{
		m_picksceneryitems.emplace_back(Track);
		model_ubs.param[0] = glm::vec4(pick_color(m_picksceneryitems.size() + 1), 1.0f);
		m_pick_shader->bind();

		draw(std::begin(Track->Geometry1), std::end(Track->Geometry1));
		draw(std::begin(Track->Geometry2), std::end(Track->Geometry2));
		if (Track->eType == tt_Switch)
			draw(Track->SwitchExtension->Geometry3);
		break;
	}
	default:
	{
		break;
	}
	}
}

// experimental, does track rendering in two passes, to take advantage of reduced texture switching
void opengl_renderer::Render(scene::basic_cell::path_sequence::const_iterator First, scene::basic_cell::path_sequence::const_iterator Last)
{

	// setup
	switch (m_renderpass.draw_mode)
	{
	case rendermode::shadows:
	{
		// NOTE: roads-based platforms tend to miss parts of shadows if rendered with either back or front culling
		glDisable(GL_CULL_FACE);
		break;
	}
	default:
	{
		break;
	}
	}

	// TODO: render auto generated trackbeds together with regular trackbeds in pass 1, and all rails in pass 2
	// first pass, material 1
	for (auto first{First}; first != Last; ++first)
	{

		auto const track{*first};

		if (track->m_material1 == 0)
		{
			continue;
		}
		if (false == track->m_visible)
		{
			continue;
		}

		++m_debugstats.paths;
		++m_debugstats.drawcalls;

		switch (m_renderpass.draw_mode)
		{
		case rendermode::color:
		case rendermode::reflections:
		{
			if (track->eEnvironment != e_flat)
			{
				setup_environment_light(track->eEnvironment);
			}
			Bind_Material(track->m_material1);
			draw(std::begin(track->Geometry1), std::end(track->Geometry1));
			if (track->eEnvironment != e_flat)
			{
				// restore default lighting
				setup_environment_light();
			}
			break;
		}
		case rendermode::shadows:
		{
			if ((std::abs(track->fTexHeight1) < 0.35f) || (track->iCategoryFlag != 2))
			{
				// shadows are only calculated for high enough roads, typically meaning track platforms
				continue;
			}
			Bind_Material_Shadow(track->m_material1);
			draw(std::begin(track->Geometry1), std::end(track->Geometry1));
			break;
		}
		case rendermode::pickscenery: // pick scenery mode uses piece-by-piece approach
		case rendermode::pickcontrols:
		default:
		{
			break;
		}
		}
	}
	// second pass, material 2
	for (auto first{First}; first != Last; ++first)
	{
		auto const track{*first};

		if (track->m_material2 == 0)
		{
			continue;
		}
		if (false == track->m_visible)
		{
			continue;
		}

		switch (m_renderpass.draw_mode)
		{
		case rendermode::color:
		case rendermode::reflections:
		{
			if (track->eEnvironment != e_flat)
			{
				setup_environment_light(track->eEnvironment);
			}
			Bind_Material(track->m_material2);
			draw(std::begin(track->Geometry2), std::end(track->Geometry2));
			if (track->eEnvironment != e_flat)
			{
				// restore default lighting
				setup_environment_light();
			}
			break;
		}
		case rendermode::shadows:
		{
			if ((std::abs(track->fTexHeight1) < 0.35f) || ((track->iCategoryFlag == 1) && (track->eType != tt_Normal)))
			{
				// shadows are only calculated for high enough trackbeds
				continue;
			}
			Bind_Material_Shadow(track->m_material2);
			draw(std::begin(track->Geometry2), std::end(track->Geometry2));
			break;
		}
		case rendermode::pickscenery: // pick scenery mode uses piece-by-piece approach
		case rendermode::pickcontrols:
		default:
		{
			break;
		}
		}
	}

	// third pass, material 3
	for (auto first{First}; first != Last; ++first)
	{

		auto const track{*first};

		if (track->eType != tt_Switch)
		{
			continue;
		}
		if (track->SwitchExtension->m_material3 == 0)
		{
			continue;
		}
		if (false == track->m_visible)
		{
			continue;
		}

		switch (m_renderpass.draw_mode)
		{
		case rendermode::color:
		case rendermode::reflections:
		{
			if (track->eEnvironment != e_flat)
			{
				setup_environment_light(track->eEnvironment);
			}
			Bind_Material(track->SwitchExtension->m_material3);
			draw(track->SwitchExtension->Geometry3);
			if (track->eEnvironment != e_flat)
			{
				// restore default lighting
				setup_environment_light();
			}
			break;
		}
		case rendermode::shadows:
		{
			if ((std::abs(track->fTexHeight1) < 0.35f) || ((track->iCategoryFlag == 1) && (track->eType != tt_Normal)))
			{
				// shadows are only calculated for high enough trackbeds
				continue;
			}
			Bind_Material_Shadow(track->SwitchExtension->m_material3);
			draw(track->SwitchExtension->Geometry3);
			break;
		}
		case rendermode::pickscenery: // pick scenery mode uses piece-by-piece approach
		case rendermode::pickcontrols:
		default:
		{
			break;
		}
		}
	}

	// post-render reset
	switch (m_renderpass.draw_mode)
	{
	case rendermode::shadows:
	{
		// restore standard face cull mode
		::glEnable(GL_CULL_FACE);
		break;
	}
	default:
	{
		break;
	}
	}
}

void opengl_renderer::Render(TMemCell *Memcell)
{

	::glPushMatrix();
	auto const position = Memcell->location() - m_renderpass.pass_camera.position();
	::glTranslated(position.x, position.y + 0.5, position.z);

	switch (m_renderpass.draw_mode)
	{
	case rendermode::color:
	{
		break;
	}
	case rendermode::shadows:
	case rendermode::pickscenery:
	{
		break;
	}
	case rendermode::reflections:
	case rendermode::pickcontrols:
	{
		break;
	}
	default:
	{
		break;
	}
	}

	::glPopMatrix();
}

void opengl_renderer::Render_precipitation()
{

	if (Global.Overcast <= 1.f)
	{
		return;
	}

	::glPushMatrix();
	// tilt the precipitation cone against the velocity vector for crude motion blur
	auto const velocity{simulation::Environment.m_precipitation.m_cameramove * -1.0};
	if (glm::length2(velocity) > 0.0)
	{
		auto const forward{glm::normalize(velocity)};
		auto left{glm::cross(forward, {0.0, 1.0, 0.0})};
		auto const rotationangle{std::min(45.0, (FreeFlyModeFlag ? 5 * glm::length(velocity) : simulation::Train->Dynamic()->GetVelocity() * 0.2))};
		::glRotated(rotationangle, left.x, 0.0, left.z);
	}
	if (false == FreeFlyModeFlag)
	{
		// counter potential vehicle roll
		auto const roll{0.5 * glm::degrees(simulation::Train->Dynamic()->Roll())};
		if (roll != 0.0)
		{
			auto const forward{simulation::Train->Dynamic()->VectorFront()};
			auto const vehicledirection = simulation::Train->Dynamic()->DirectionGet();
			::glRotated(roll, forward.x, 0.0, forward.z);
		}
	}
	if (!Global.iPause)
	{
		if (Global.Weather == "rain:")
			// oddly enough random streaks produce more natural looking rain than ones the eye can follow
			m_precipitationrotation = LocalRandom() * 360;
		else
			m_precipitationrotation = 0.0;
	}

	::glRotated(m_precipitationrotation, 0.0, 1.0, 0.0);

	model_ubs.set_modelview(OpenGLMatrices.data(GL_MODELVIEW));
	model_ubs.param[0] = interpolate(0.5f * (Global.DayLight.diffuse + Global.DayLight.ambient), colors::white, 0.5f * clamp<float>(Global.fLuminance, 0.f, 1.f));
	model_ubs.param[1].x = simulation::Environment.m_precipitation.get_textureoffset();
	model_ubo->update(model_ubs);

	// momentarily disable depth write, to allow vehicle cab drawn afterwards to mask it instead of leaving it 'inside'
	::glDepthMask(GL_FALSE);

	simulation::Environment.m_precipitation.render();

	::glDepthMask(GL_TRUE);
	::glPopMatrix();
}

void opengl_renderer::Render_Alpha(scene::basic_region *Region)
{

	// sort the nodes based on their distance to viewer
	std::sort(std::begin(m_cellqueue), std::end(m_cellqueue), [](distancecell_pair const &Left, distancecell_pair const &Right) { return (Left.first) < (Right.first); });

	Render_Alpha(std::rbegin(m_cellqueue), std::rend(m_cellqueue));
}

void opengl_renderer::Render_Alpha(cell_sequence::reverse_iterator First, cell_sequence::reverse_iterator Last)
{

	// NOTE: this method is launched only during color pass therefore we don't bother with mode test here
	// first pass draws elements which we know are located in section banks, to reduce vbo switching
	{
		auto first{First};
		while (first != Last)
		{

			auto const *cell = first->second;

			if (false == cell->m_shapestranslucent.empty())
			{
				// since all shapes of the cell share center point we can optimize out a few calls here
				::glPushMatrix();
				auto const originoffset{cell->m_area.center - m_renderpass.pass_camera.position()};
				::glTranslated(originoffset.x, originoffset.y, originoffset.z);
				// render
				// NOTE: we can reuse the method used to draw opaque geometry
				for (auto const &shape : cell->m_shapestranslucent)
				{
					Render(shape, false);
				}
				// post-render cleanup
				::glPopMatrix();
			}

			++first;
		}
	}
	// second pass draws elements with their own vbos
	{
		auto first{First};
		while (first != Last)
		{

			auto const *cell = first->second;

			// translucent parts of instanced models
			for (auto *instance : cell->m_instancetranslucent)
			{
				Render_Alpha(instance);
			}
			// translucent parts of vehicles
			for (auto *path : cell->m_paths)
			{
				for (auto *dynamic : path->Dynamics)
				{
					Render_Alpha(dynamic);
				}
			}

			++first;
		}
	}
	// third pass draws the wires;
	// wires use section vbos, but for the time being we want to draw them at the very end
	{
		auto first{First};
		while (first != Last)
		{

			auto const *cell = first->second;

			if ((false == cell->m_traction.empty() || (false == cell->m_lines.empty())))
			{
				// since all shapes of the cell share center point we can optimize out a few calls here
				::glPushMatrix();
				auto const originoffset{cell->m_area.center - m_renderpass.pass_camera.position()};
				::glTranslated(originoffset.x, originoffset.y, originoffset.z);
				Bind_Material(null_handle);
				// render
				for (auto *traction : cell->m_traction)
				{
					Render_Alpha(traction);
				}
				for (auto &lines : cell->m_lines)
				{
					Render_Alpha(lines);
				}
				// post-render cleanup
				::glPopMatrix();
			}

			++first;
		}
	}
}

void opengl_renderer::Render_Alpha(TAnimModel *Instance)
{

	if (false == Instance->m_visible)
	{
		return;
	}

	double distancesquared;
	switch (m_renderpass.draw_mode)
	{
	case rendermode::shadows:
	default:
	{
		distancesquared = glm::length2((Instance->location() - m_renderpass.pass_camera.position()) / (double)Global.ZoomFactor) / Global.fDistanceFactor;
		break;
	}
	}
	if ((distancesquared < Instance->m_rangesquaredmin) || (distancesquared >= Instance->m_rangesquaredmax))
	{
		return;
	}

	Instance->RaPrepare();
	if (Instance->pModel)
	{
		// renderowanie rekurencyjne submodeli
		Render_Alpha(Instance->pModel, Instance->Material(), distancesquared, Instance->location() - m_renderpass.pass_camera.position(), Instance->vAngle);
	}
}

void opengl_renderer::Render_Alpha(TTraction *Traction)
{
	glDebug("Render_Alpha TTraction");

	auto const distancesquared { glm::length2( ( Traction->location() - m_renderpass.pass_camera.position() ) / (double)Global.ZoomFactor ) / Global.fDistanceFactor };
	if ((distancesquared < Traction->m_rangesquaredmin) || (distancesquared >= Traction->m_rangesquaredmax))
	{
		return;
	}

	if (false == Traction->m_visible)
	{
		return;
	}
	// rysuj jesli sa druty i nie zerwana
	if ((Traction->Wires == 0) || (true == TestFlag(Traction->DamageFlag, 128)))
	{
		return;
	}
	// setup
	auto const distance{static_cast<float>(std::sqrt(distancesquared))};
	auto const linealpha = 20.f * Traction->WireThickness / std::max(0.5f * Traction->radius() + 1.f, distance - (0.5f * Traction->radius()));
	if (m_widelines_supported)
		glLineWidth(clamp(0.5f * linealpha + Traction->WireThickness * Traction->radius() / 1000.f, 1.f, 1.75f));

	// render

	// McZapkie-261102: kolor zalezy od materialu i zasniedzenia
	model_ubs.param[0] = glm::vec4(Traction->wire_color(), glm::min(1.0f, linealpha));

	if (m_renderpass.draw_mode == rendermode::shadows)
		Bind_Material_Shadow(null_handle);
	else
		m_line_shader->bind();

	draw(Traction->m_geometry);

	// debug data
	++m_debugstats.traction;
	++m_debugstats.drawcalls;
}

void opengl_renderer::Render_Alpha(scene::lines_node const &Lines)
{
	glDebug("Render_Alpha scene::lines_node");

	auto const &data{Lines.data()};

	auto const distancesquared { glm::length2( ( data.area.center - m_renderpass.pass_camera.position() ) / (double)Global.ZoomFactor ) / Global.fDistanceFactor };
	if ((distancesquared < data.rangesquared_min) || (distancesquared >= data.rangesquared_max))
	{
		return;
	}
	// setup
	auto const distance{static_cast<float>(std::sqrt(distancesquared))};
	auto const linealpha =
	    (data.line_width > 0.f ? 10.f * data.line_width / std::max(0.5f * data.area.radius + 1.f, distance - (0.5f * data.area.radius)) : 1.f); // negative width means the lines are always opague
	if (m_widelines_supported)
		glLineWidth(clamp(0.5f * linealpha + data.line_width * data.area.radius / 1000.f, 1.f, 8.f));

	model_ubs.param[0] = glm::vec4(glm::vec3(data.lighting.diffuse * m_sunlight.ambient), glm::min(1.0f, linealpha));

	if (m_renderpass.draw_mode == rendermode::shadows)
		Bind_Material_Shadow(null_handle);
	else
		m_line_shader->bind();
	draw(data.geometry);

	++m_debugstats.lines;
	++m_debugstats.drawcalls;
}

bool opengl_renderer::Render_Alpha(TDynamicObject *Dynamic)
{

	if (false == Dynamic->renderme)
	{
		return false;
	}

	// setup
	TSubModel::iInstance = (size_t)Dynamic; //żeby nie robić cudzych animacji
	glm::dvec3 const originoffset = Dynamic->vPosition - m_renderpass.pass_camera.position();
	// lod visibility ranges are defined for base (x 1.0) viewing distance. for render we adjust them for actual range multiplier and zoom
	float squaredistance;
	switch (m_renderpass.draw_mode)
	{
	case rendermode::shadows:
	default:
	{
		squaredistance = glm::length2(glm::vec3{originoffset} / Global.ZoomFactor) / Global.fDistanceFactor;
		break;
	}
	}
	Dynamic->ABuLittleUpdate(squaredistance); // ustawianie zmiennych submodeli dla wspólnego modelu

	glm::mat4 future_stack = model_ubs.future;

	glm::mat4 mv = OpenGLMatrices.data(GL_MODELVIEW);
	model_ubs.future *= glm::translate(mv, glm::vec3(Dynamic->get_future_movement())) * glm::inverse(mv);

	::glPushMatrix();

	::glTranslated(originoffset.x, originoffset.y, originoffset.z);
	::glMultMatrixd(Dynamic->mMatrix.getArray());

	if (Dynamic->fShade > 0.0f)
	{
		// change light level based on light level of the occupied track
		m_sunlight.apply_intensity(Dynamic->fShade);
	}

	// render
	if (Dynamic->mdLowPolyInt)
	{
		// low poly interior
		Render_Alpha(Dynamic->mdLowPolyInt, Dynamic->Material(), squaredistance);
	}

	if (Dynamic->mdModel)
		Render_Alpha(Dynamic->mdModel, Dynamic->Material(), squaredistance);

	if (Dynamic->mdLoad) // renderowanie nieprzezroczystego ładunku
		Render_Alpha(Dynamic->mdLoad, Dynamic->Material(), squaredistance);

	// post-render cleanup
	if (Dynamic->fShade > 0.0f)
	{
		// restore regular light level
		m_sunlight.apply_intensity();
	}

	::glPopMatrix();

	model_ubs.future = future_stack;

	if (Dynamic->btnOn)
		Dynamic->TurnOff(); // przywrócenie domyślnych pozycji submodeli

	return true;
}

bool opengl_renderer::Render_Alpha(TModel3d *Model, material_data const *Material, float const Squaredistance)
{

	auto alpha = (Material != nullptr ? Material->textures_alpha : 0x30300030);

	if (0 == (alpha & Model->iFlags & 0x2F2F002F))
	{
		// nothing to render
		return false;
	}

	Model->Root->fSquareDist = Squaredistance; // zmienna globalna!

	// setup
	Model->Root->ReplacableSet((Material != nullptr ? Material->replacable_skins : nullptr), alpha);

	Model->Root->pRoot = Model;

	// render
	Render_Alpha(Model->Root);

	// post-render cleanup

	return true;
}

bool opengl_renderer::Render_Alpha(TModel3d *Model, material_data const *Material, float const Squaredistance, Math3D::vector3 const &Position, glm::vec3 const &A)
{
	Math3D::vector3 Angle(A);
	::glPushMatrix();
	::glTranslated(Position.x, Position.y, Position.z);
	if (Angle.y != 0.0)
		::glRotated(Angle.y, 0.0, 1.0, 0.0);
	if (Angle.x != 0.0)
		::glRotated(Angle.x, 1.0, 0.0, 0.0);
	if (Angle.z != 0.0)
		::glRotated(Angle.z, 0.0, 0.0, 1.0);

	auto const result = Render_Alpha(Model, Material, Squaredistance); // position is effectively camera offset

	::glPopMatrix();

	return result;
}

void opengl_renderer::Render_Alpha(TSubModel *Submodel)
{
	// renderowanie przezroczystych przez DL
	if ((Submodel->iVisible) && (TSubModel::fSquareDist >= Submodel->fSquareMinDist) && (TSubModel::fSquareDist < Submodel->fSquareMaxDist))
	{

		// debug data
		++m_debugstats.submodels;
		++m_debugstats.drawcalls;

		glm::mat4 future_stack = model_ubs.future;

		if (Submodel->iFlags & 0xC000)
		{
			::glPushMatrix();
			if (Submodel->fMatrix)
				::glMultMatrixf(Submodel->fMatrix->readArray());
			if (Submodel->b_aAnim != TAnimType::at_None)
			{
				Submodel->RaAnimation(Submodel->b_aAnim);

				glm::mat4 mv = OpenGLMatrices.data(GL_MODELVIEW);
				model_ubs.future *= (mv * Submodel->future_transform) * glm::inverse(mv);
			}
		}

		if (Submodel->eType < TP_ROTATOR)
		{
			// renderowanie obiektów OpenGL
			if (Submodel->iAlpha & Submodel->iFlags & 0x2F)
			{
				// rysuj gdy element przezroczysty
				switch (m_renderpass.draw_mode)
				{
				case rendermode::color:
				{
					// material configuration:
					// textures...
					if (Submodel->m_material < 0)
					{ // zmienialne skóry
						Bind_Material(Submodel->ReplacableSkinId[-Submodel->m_material], Submodel);
					}
					else
					{
						Bind_Material(Submodel->m_material, Submodel);
					}
					// ...luminance

					auto const isemissive { ( Submodel->f4Emision.a > 0.f ) && ( Global.fLuminance < Submodel->fLight ) };
					if (isemissive)
						model_ubs.emission = Submodel->f4Emision.a;

					// main draw call
					draw(Submodel->m_geometry);

					model_ubs.emission = 0.0f;
					break;
				}
				case rendermode::cabshadows:
				{
					if (Submodel->m_material < 0)
					{ // zmienialne skóry
						Bind_Material_Shadow(Submodel->ReplacableSkinId[-Submodel->m_material]);
					}
					else
					{
						Bind_Material_Shadow(Submodel->m_material);
					}
					draw(Submodel->m_geometry);
					break;
				}
				default:
				{
					break;
				}
				}
			}
		}
		else if (Submodel->eType == TP_FREESPOTLIGHT)
		{
			if (Global.fLuminance < Submodel->fLight || Global.Overcast > 1.0f)
			{
				// NOTE: we're forced here to redo view angle calculations etc, because this data isn't instanced but stored along with the single mesh
				// TODO: separate instance data from reusable geometry
				auto const &modelview = OpenGLMatrices.data(GL_MODELVIEW);
				auto const lightcenter = modelview * interpolate(glm::vec4(0.f, 0.f, -0.05f, 1.f), glm::vec4(0.f, 0.f, -0.10f, 1.f),
				                                                 static_cast<float>(TSubModel::fSquareDist / Submodel->fSquareMaxDist)); // pozycja punktu świecącego względem kamery
				Submodel->fCosViewAngle = glm::dot(glm::normalize(modelview * glm::vec4(0.f, 0.f, -1.f, 1.f) - lightcenter), glm::normalize(-lightcenter));

				if (Submodel->fCosViewAngle > Submodel->fCosFalloffAngle)
				{
					// only bother if the viewer is inside the visibility cone
					// luminosity at night is at level of ~0.1, so the overall resulting transparency in clear conditions is ~0.5 at full 'brightness'
					auto glarelevel{clamp(std::max<float>(0.6f - Global.fLuminance, // reduce the glare in bright daylight
						                                  Global.Overcast - 1.f), // ensure some glare in rainy/foggy conditions
						                  0.f, 1.f)};
					// view angle attenuation
					float const anglefactor{clamp((Submodel->fCosViewAngle - Submodel->fCosFalloffAngle) / (Submodel->fCosHotspotAngle - Submodel->fCosFalloffAngle), 0.f, 1.f)};

					glarelevel *= anglefactor;

					if (glarelevel > 0.0f)
					{
						glDepthMask(GL_FALSE);
						glBlendFunc(GL_SRC_ALPHA, GL_ONE);

						::glPushMatrix();
						::glLoadIdentity(); // macierz jedynkowa
						::glTranslatef(lightcenter.x, lightcenter.y, lightcenter.z); // początek układu zostaje bez zmian
						::glRotated(std::atan2(lightcenter.x, lightcenter.z) * 180.0 / M_PI, 0.0, 1.0, 0.0); // jedynie obracamy w pionie o kąt

						auto const lightcolor = glm::vec3(Submodel->DiffuseOverride.r < 0.f ? // -1 indicates no override
						                           Submodel->f4Diffuse :
						                           Submodel->DiffuseOverride);

						m_billboard_shader->bind();
						Bind_Texture(0, m_glaretexture);
						model_ubs.param[0] = glm::vec4(glm::vec3(lightcolor), Submodel->fVisible * glarelevel);

						// main draw call
						draw(m_billboardgeometry);

						glDepthMask(GL_TRUE);
						glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

						::glPopMatrix();
					}
				}
			}
		}

		if (Submodel->Child != nullptr)
		{
			if (Submodel->eType == TP_TEXT)
			{ // tekst renderujemy w specjalny sposób, zamiast submodeli z łańcucha Child
				int i, j = (int)Submodel->pasText->size();
				TSubModel *p;
				if (!Submodel->smLetter)
				{ // jeśli nie ma tablicy, to ją stworzyć; miejsce nieodpowiednie, ale tymczasowo może być
					Submodel->smLetter = new TSubModel *[256]; // tablica wskaźników submodeli dla wyświetlania tekstu
					memset(Submodel->smLetter, 0, 256 * sizeof(TSubModel *)); // wypełnianie zerami
					p = Submodel->Child;
					while (p)
					{
						Submodel->smLetter[p->pName[0]] = p;
						p = p->Next; // kolejny znak
					}
				}
				for (i = 1; i <= j; ++i)
				{
					p = Submodel->smLetter[(*(Submodel->pasText))[i]]; // znak do wyświetlenia
					if (p)
					{ // na razie tylko jako przezroczyste
						Render_Alpha(p);
						if (p->fMatrix)
							::glMultMatrixf(p->fMatrix->readArray()); // przesuwanie widoku
					}
				}
			}
			else if (Submodel->iAlpha & Submodel->iFlags & 0x002F0000)
				Render_Alpha(Submodel->Child);
		}

		if (Submodel->iFlags & 0xC000)
		{
			model_ubs.future = future_stack;
			::glPopMatrix();
		}
	}
	/*
	    if( Submodel->b_aAnim < at_SecondsJump )
	        Submodel->b_aAnim = at_None; // wyłączenie animacji dla kolejnego użycia submodelu
	*/
	if (Submodel->Next != nullptr)
		if (Submodel->iAlpha & Submodel->iFlags & 0x2F000000)
			Render_Alpha(Submodel->Next);
};

// utility methods
void opengl_renderer::Update_Pick_Control()
{
	// context-switch workaround
	gl::buffer::unbind();

	if (!m_picking_pbo->is_busy())
	{
		unsigned char pickreadout[4];
		if (m_picking_pbo->read_data(1, 1, pickreadout))
		{
			auto const controlindex = pick_index(glm::ivec3{pickreadout[0], pickreadout[1], pickreadout[2]});
			TSubModel const *control{nullptr};
			if ((controlindex > 0) && (controlindex <= m_pickcontrolsitems.size()))
			{
				control = m_pickcontrolsitems[controlindex - 1];
			}

			m_pickcontrolitem = control;

			for (auto f : m_control_pick_requests)
				f(m_pickcontrolitem);
			m_control_pick_requests.clear();
		}

		if (!m_control_pick_requests.empty())
		{
			// determine point to examine
			glm::dvec2 mousepos = Application.get_cursor_pos();
			mousepos.y = Global.iWindowHeight - mousepos.y; // cursor coordinates are flipped compared to opengl

			glm::ivec2 pickbufferpos;
			pickbufferpos = glm::ivec2{mousepos.x * EU07_PICKBUFFERSIZE / std::max(1, Global.iWindowWidth), mousepos.y * EU07_PICKBUFFERSIZE / std::max(1, Global.iWindowHeight)};
			pickbufferpos = glm::clamp(pickbufferpos, glm::ivec2(0, 0), glm::ivec2(EU07_PICKBUFFERSIZE - 1, EU07_PICKBUFFERSIZE - 1));

			Render_pass(*m_viewports.front().get(), rendermode::pickcontrols);
			m_pick_fb->bind();
			m_picking_pbo->request_read(pickbufferpos.x, pickbufferpos.y, 1, 1);
			m_pick_fb->unbind();
		}
	}
}

void opengl_renderer::Update_Pick_Node()
{
	if (!m_picking_node_pbo->is_busy())
	{
		unsigned char pickreadout[4];
		if (m_picking_node_pbo->read_data(1, 1, pickreadout))
		{
			auto const nodeindex = pick_index(glm::ivec3{pickreadout[0], pickreadout[1], pickreadout[2]});
			scene::basic_node *node{nullptr};
			if ((nodeindex > 0) && (nodeindex <= m_picksceneryitems.size()))
			{
				node = m_picksceneryitems[nodeindex - 1];
			}

			m_picksceneryitem = node;

			for (auto f : m_node_pick_requests)
				f(m_picksceneryitem);
			m_node_pick_requests.clear();
		}

		if (!m_node_pick_requests.empty())
		{
			// determine point to examine
			glm::dvec2 mousepos = Application.get_cursor_pos();
			mousepos.y = Global.iWindowHeight - mousepos.y; // cursor coordinates are flipped compared to opengl

			glm::ivec2 pickbufferpos;
			pickbufferpos = glm::ivec2{mousepos.x * EU07_PICKBUFFERSIZE / std::max(1, Global.iWindowWidth), mousepos.y * EU07_PICKBUFFERSIZE / std::max(1, Global.iWindowHeight)};
			pickbufferpos = glm::clamp(pickbufferpos, glm::ivec2(0, 0), glm::ivec2(EU07_PICKBUFFERSIZE - 1, EU07_PICKBUFFERSIZE - 1));

			Render_pass(*m_viewports.front().get(), rendermode::pickscenery);
			m_pick_fb->bind();
			m_picking_node_pbo->request_read(pickbufferpos.x, pickbufferpos.y, 1, 1);
			m_pick_fb->unbind();
		}
	}
}

void opengl_renderer::pick_control(std::function<void(TSubModel const *)> callback)
{
	m_control_pick_requests.push_back(callback);
}

void opengl_renderer::pick_node(std::function<void(scene::basic_node *)> callback)
{
	m_node_pick_requests.push_back(callback);
}

glm::dvec3 opengl_renderer::get_mouse_depth()
{
	if (!m_depth_pointer_pbo->is_busy())
	{
		// determine point to examine
		glm::dvec2 mousepos = Application.get_cursor_pos();
		mousepos.y = Global.iWindowHeight - mousepos.y; // cursor coordinates are flipped compared to opengl

		glm::ivec2 bufferpos;
		bufferpos = glm::ivec2{mousepos.x * Global.gfx_framebuffer_width / std::max(1, Global.iWindowWidth), mousepos.y * Global.gfx_framebuffer_height / std::max(1, Global.iWindowHeight)};
		bufferpos = glm::clamp(bufferpos, glm::ivec2(0, 0), glm::ivec2(Global.gfx_framebuffer_width - 1, Global.gfx_framebuffer_height - 1));

		float pointdepth = std::numeric_limits<float>::max();

		if (!Global.gfx_usegles)
		{
			m_depth_pointer_pbo->read_data(1, 1, &pointdepth, 4);

			if (!Global.iMultisampling)
			{
				m_depth_pointer_pbo->request_read(bufferpos.x, bufferpos.y, 1, 1, 4, GL_DEPTH_COMPONENT, GL_FLOAT);
			}
			else if (Global.gfx_skippipeline)
			{
				gl::framebuffer::blit(nullptr, m_depth_pointer_fb.get(), bufferpos.x, bufferpos.y, 1, 1, GL_DEPTH_BUFFER_BIT, 0);

				m_depth_pointer_fb->bind();
				m_depth_pointer_pbo->request_read(0, 0, 1, 1, 4, GL_DEPTH_COMPONENT, GL_FLOAT);
				m_depth_pointer_fb->unbind();
			}
			else
			{
				gl::framebuffer::blit(m_viewports.front()->msaa_fb.get(), m_depth_pointer_fb.get(), bufferpos.x, bufferpos.y, 1, 1, GL_DEPTH_BUFFER_BIT, 0);

				m_depth_pointer_fb->bind();
				m_depth_pointer_pbo->request_read(0, 0, 1, 1, 4, GL_DEPTH_COMPONENT, GL_FLOAT);
				m_viewports.front()->msaa_fb->bind();
			}
		}
		else
		{
			unsigned int data[4];
			if (m_depth_pointer_pbo->read_data(1, 1, data, 16))
				pointdepth = (double)data[0] / 65535.0;

			if (Global.gfx_skippipeline)
			{
				gl::framebuffer::blit(nullptr, m_depth_pointer_fb.get(), 0, 0, Global.gfx_framebuffer_width, Global.gfx_framebuffer_height, GL_DEPTH_BUFFER_BIT, 0);

				m_empty_vao->bind();
				m_depth_pointer_tex->bind(0);
				m_depth_pointer_shader->bind();
				m_depth_pointer_fb2->bind();
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
				m_depth_pointer_pbo->request_read(bufferpos.x, bufferpos.y, 1, 1, 16, GL_RGBA_INTEGER, GL_UNSIGNED_INT);
				m_depth_pointer_shader->unbind();
				m_empty_vao->unbind();
				m_depth_pointer_fb2->unbind();
			}
			else
			{
				gl::framebuffer::blit(m_viewports.front()->msaa_fb.get(), m_depth_pointer_fb.get(), 0, 0, Global.gfx_framebuffer_width, Global.gfx_framebuffer_height, GL_DEPTH_BUFFER_BIT, 0);

				m_empty_vao->bind();
				m_depth_pointer_tex->bind(0);
				m_depth_pointer_shader->bind();
				m_depth_pointer_fb2->bind();
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
				m_depth_pointer_pbo->request_read(bufferpos.x, bufferpos.y, 1, 1, 16, GL_RGBA_INTEGER, GL_UNSIGNED_INT);
				m_depth_pointer_shader->unbind();
				m_empty_vao->unbind();
				m_viewports.front()->msaa_fb->bind();
			}
		}

		if (pointdepth != std::numeric_limits<float>::max())
		{
			if (GLAD_GL_ARB_clip_control || GLAD_GL_EXT_clip_control) {
				if (pointdepth > 0.0f)
					m_worldmousecoordinates = glm::unProjectZO(glm::vec3(bufferpos, pointdepth), glm::mat4(glm::mat3(m_colorpass.pass_camera.modelview())), m_colorpass.pass_camera.projection(),
					                                           glm::vec4(0, 0, Global.gfx_framebuffer_width, Global.gfx_framebuffer_height));
			} else if (pointdepth < 1.0f)
				m_worldmousecoordinates = glm::unProjectNO(glm::vec3(bufferpos, pointdepth), glm::mat4(glm::mat3(m_colorpass.pass_camera.modelview())), m_colorpass.pass_camera.projection(),
				                                           glm::vec4(0, 0, Global.gfx_framebuffer_width, Global.gfx_framebuffer_height));
		}
	}

	return m_colorpass.pass_camera.position() + glm::dvec3{m_worldmousecoordinates};
}

void opengl_renderer::Update(double const Deltatime)
{
	Update_Pick_Control();
	Update_Pick_Node();

	m_updateaccumulator += Deltatime;

	if (m_updateaccumulator < 1.0)
	{
		// too early for any work
		return;
	}

	m_updateaccumulator = 0.0;
	m_framerate = 1000.f / (Timer::subsystem.mainloop_total.average());

	// adjust draw ranges etc, based on recent performance
	// TODO: it doesn't make much sense with vsync

	if (Global.targetfps != 0.0f) {
		float fps_diff = Global.targetfps - m_framerate;
		if (fps_diff > 0.0f)
			Global.fDistanceFactor = std::max(0.5f, Global.fDistanceFactor - 0.05f);
		else
			Global.fDistanceFactor = std::min(3.0f, Global.fDistanceFactor + 0.05f);
	}

	if ((true == Global.ResourceSweep) && (true == simulation::is_ready))
	{
		// garbage collection
		m_geometry.update();
		m_textures.update();
	}

	if ((true == Global.ControlPicking) && (false == FreeFlyModeFlag))
		pick_control([](const TSubModel *) {});
	// temporary conditions for testing. eventually will be coupled with editor mode
	if ((true == Global.ControlPicking) && (true == DebugModeFlag) && (true == FreeFlyModeFlag))
		pick_node([](scene::basic_node *) {});

	// dump last opengl error, if any
	auto const glerror = ::glGetError();
	if (glerror != GL_NO_ERROR)
	{
		std::string glerrorstring;
		if (glerror == GL_INVALID_ENUM)
			glerrorstring = "GL_INVALID_ENUM";
		else if (glerror == GL_INVALID_VALUE)
			glerrorstring = "GL_INVALID_VALUE";
		else if (glerror == GL_INVALID_OPERATION)
			glerrorstring = "GL_INVALID_OPERATION";
		else if (glerror == GL_OUT_OF_MEMORY)
			glerrorstring = "GL_OUT_OF_MEMORY";
		else if (glerror == GL_INVALID_FRAMEBUFFER_OPERATION)
			glerrorstring = "GL_INVALID_FRAMEBUFFER_OPERATION";
		Global.LastGLError = std::to_string(glerror) + " (" + glerrorstring + ")";
	}
}

// debug performance string
std::string const &opengl_renderer::info_times() const
{

	return m_debugtimestext;
}

std::string const &opengl_renderer::info_stats() const
{

	return m_debugstatstext;
}

void opengl_renderer::Update_Lights(light_array &Lights)
{
	glDebug("Update_Lights");

	// arrange the light array from closest to farthest from current position of the camera
	auto const camera = m_renderpass.pass_camera.position();
	std::sort(std::begin(Lights.data), std::end(Lights.data), [&camera](light_array::light_record const &Left, light_array::light_record const &Right) {
		// move lights which are off at the end...
		if (Left.intensity == 0.f)
		{
			return false;
		}
		if (Right.intensity == 0.f)
		{
			return true;
		}
		// ...otherwise prefer closer and/or brigher light sources
		return (glm::length2(camera - Left.position) * (1.f - Left.intensity)) < (glm::length2(camera - Right.position) * (1.f - Right.intensity));
	});

	auto renderlight = m_lights.begin();
	size_t light_i = 1;

	glm::mat4 mv = OpenGLMatrices.data(GL_MODELVIEW);

	for (auto const &scenelight : Lights.data)
	{

		if (renderlight == m_lights.end())
		{
			// we ran out of lights to assign
			break;
		}
		if (scenelight.intensity == 0.f)
		{
			// all lights past this one are bound to be off
			break;
		}
		auto const lightoffset = glm::vec3{scenelight.position - camera};
		if (glm::length(lightoffset) > 1000.f)
		{
			// we don't care about lights past arbitrary limit of 1 km.
			// but there could still be weaker lights which are closer, so keep looking
			continue;
		}
		// if the light passed tests so far, it's good enough
		renderlight->position = lightoffset;
		renderlight->direction = scenelight.direction;

		auto luminance = static_cast<float>(Global.fLuminance);
		// adjust luminance level based on vehicle's location, e.g. tunnels
		auto const environment = scenelight.owner->fShade;
		if (environment > 0.f)
		{
			luminance *= environment;
		}
		renderlight->diffuse = glm::vec4{glm::max(glm::vec3{colors::none}, scenelight.color - glm::vec3{luminance}), renderlight->diffuse[3]};
		renderlight->ambient = glm::vec4{glm::max(glm::vec3{colors::none}, scenelight.color * glm::vec3{scenelight.intensity} - glm::vec3{luminance}), renderlight->ambient[3]};

		renderlight->apply_intensity();
		renderlight->apply_angle();

		gl::light_element_ubs *l = &light_ubs.lights[light_i];
		l->pos = mv * glm::vec4(renderlight->position, 1.0f);
		l->dir = mv * glm::vec4(renderlight->direction, 0.0f);
		l->type = gl::light_element_ubs::SPOT;
		l->in_cutoff = headlight_config.in_cutoff;
		l->out_cutoff = headlight_config.out_cutoff;
		l->color = renderlight->diffuse * renderlight->factor;
		l->linear = headlight_config.falloff_linear / 10.0f;
		l->quadratic = headlight_config.falloff_quadratic / 100.0f;
		l->ambient = headlight_config.ambient;
		l->intensity = headlight_config.intensity;
		light_i++;

		++renderlight;
	}

	light_ubs.ambient = m_sunlight.ambient * m_sunlight.factor;
	light_ubs.lights[0].type = gl::light_element_ubs::DIR;
	light_ubs.lights[0].dir = mv * glm::vec4(m_sunlight.direction, 0.0f);
	light_ubs.lights[0].color = m_sunlight.diffuse * m_sunlight.factor;
	light_ubs.lights[0].ambient = 0.0f;
	light_ubs.lights[0].intensity = 1.0f;
	light_ubs.lights_count = light_i;

	light_ubs.fog_color = Global.FogColor;
	if (Global.fFogEnd > 0)
	{
		m_fogrange = Global.fFogEnd / std::max(1.f, Global.Overcast * 2.f);
		model_ubs.fog_density = 1.0f / m_fogrange;
	}
	else
		model_ubs.fog_density = 0.0f;

	model_ubo->update(model_ubs);
	light_ubo->update(light_ubs);
}

bool opengl_renderer::Init_caps()
{
	WriteLog("MaSzyna OpenGL Renderer");
	WriteLog("Renderer: " + std::string((char *)glGetString(GL_RENDERER)));
	WriteLog("Vendor: " + std::string((char *)glGetString(GL_VENDOR)));
	WriteLog("GL version: " + std::string((char *)glGetString(GL_VERSION)));

	WriteLog("--------");

	GLint extCount = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &extCount);

	WriteLog("Supported extensions:");
	for (int i = 0; i < extCount; i++)
	{
		const char *ext = (const char *)glGetStringi(GL_EXTENSIONS, i);
		WriteLog(ext);
	}
	WriteLog("--------");

	if (!Global.gfx_usegles)
	{
		if (!GLAD_GL_VERSION_3_3)
		{
			ErrorLog("requires OpenGL >= 3.3!");
			return false;
		}

		if (!GLAD_GL_EXT_texture_sRGB)
			ErrorLog("EXT_texture_sRGB not supported!");

		if (!GLAD_GL_EXT_texture_compression_s3tc)
			ErrorLog("EXT_texture_compression_s3tc not supported!");

		if (GLAD_GL_ARB_texture_filter_anisotropic)
			WriteLog("ARB_texture_filter_anisotropic supported!");

		if (GLAD_GL_ARB_multi_bind)
			WriteLog("ARB_multi_bind supported!");

		if (GLAD_GL_ARB_direct_state_access)
			WriteLog("ARB_direct_state_access supported!");

		if (GLAD_GL_ARB_clip_control)
			WriteLog("ARB_clip_control supported!");
	}
	else
	{
		if (!GLAD_GL_ES_VERSION_3_0)
		{
			ErrorLog("requires OpenGL ES >= 3.0!");
			return false;
		}

		if (GLAD_GL_EXT_texture_filter_anisotropic)
			WriteLog("EXT_texture_filter_anisotropic supported!");

		if (GLAD_GL_EXT_clip_control)
			WriteLog("EXT_clip_control supported!");

		if (GLAD_GL_EXT_geometry_shader)
			WriteLog("EXT_geometry_shader supported!");
	}

	glGetError();
	glLineWidth(2.0f);
	if (!glGetError())
	{
		WriteLog("wide lines supported!");
		m_widelines_supported = true;
	}
	else
		WriteLog("warning: wide lines not supported");

	WriteLog("--------");

	// ograniczenie maksymalnego rozmiaru tekstur - parametr dla skalowania tekstur
	{
		GLint texturesize;
		::glGetIntegerv(GL_MAX_TEXTURE_SIZE, &texturesize);
		Global.iMaxTextureSize = std::min(Global.iMaxTextureSize, texturesize);
		WriteLog("texture sizes capped at " + std::to_string(Global.iMaxTextureSize) + "px");
		m_shadowbuffersize = Global.shadowtune.map_size;
		m_shadowbuffersize = std::min(m_shadowbuffersize, texturesize);
		WriteLog("shadows map size capped at " + std::to_string(m_shadowbuffersize) + "px");
	}
	Global.DynamicLightCount = std::min(Global.DynamicLightCount, 8);

	if (Global.iMultisampling)
	{
		WriteLog("using multisampling x" + std::to_string(1 << Global.iMultisampling));
	}

	if (Global.gfx_framebuffer_width == -1)
		Global.gfx_framebuffer_width = Global.iWindowWidth;
	if (Global.gfx_framebuffer_height == -1)
		Global.gfx_framebuffer_height = Global.iWindowHeight;

	WriteLog("main window size: " + std::to_string(Global.gfx_framebuffer_width) + "x" + std::to_string(Global.gfx_framebuffer_height));

	return true;
}

glm::vec3 opengl_renderer::pick_color(std::size_t const Index)
{
	return glm::vec3{((Index & 0xff0000) >> 16) / 255.0f, ((Index & 0x00ff00) >> 8) / 255.0f, (Index & 0x0000ff) / 255.0f};
}

std::size_t opengl_renderer::pick_index(glm::ivec3 const &Color)
{
	return Color.b + (Color.g * 256) + (Color.r * 256 * 256);
}

//---------------------------------------------------------------------------
