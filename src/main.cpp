#define _USE_MATH_DEFINES
#include <application.h>
#include <mesh.h>
#include <camera.h>
#include <material.h>
#include <memory>
#include <iostream>
#include <stack>
#include <random>
#include <chrono>
#include <random>
#include <rtccore.h>
#include <rtcore_geometry.h>
#include <rtcore_common.h>
#include <rtcore_ray.h>
#include <rtcore_device.h>
#include <rtcore_scene.h>

#define CAMERA_FAR_PLANE 10000.0f
#define PROJECTOR_BACK_OFF_DISTANCE 10.0f
#define ALBEDO_TEXTURE_SIZE 4096
#define DEPTH_TEXTURE_SIZE 512

struct GlobalUniforms
{
    DW_ALIGNED(16)
    glm::mat4 view_proj;
    DW_ALIGNED(16)
    glm::mat4 inv_view_proj;
    DW_ALIGNED(16)
    glm::vec4 cam_pos;
};

struct DecalInstance
{
    // Last hit
    glm::vec3 m_hit_pos;
    glm::vec3 m_hit_normal;
    float     m_hit_distance = INFINITY;

    // Projector
    glm::vec3 m_projector_pos;
    glm::vec3 m_projector_dir;
    glm::mat4 m_projector_view;
    glm::mat4 m_projector_view_proj;
    glm::mat4 m_projector_proj;

    // Debug
    int32_t m_selected_decal = 0;
};

class DeferredDecals : public dw::Application
{
protected:
    // -----------------------------------------------------------------------------------------------------------------------------------

    bool init(int argc, const char* argv[]) override
    {
        // Create GPU resources.
        if (!create_shaders())
            return false;

        if (!create_uniform_buffer())
            return false;

        // Load scene.
        if (!load_scene())
            return false;

        if (!load_decals())
            return false;

        if (!initialize_embree())
            return false;

        create_cube();
        create_textures();
        create_framebuffers();

        // Create camera.
        create_camera();

        m_transform = glm::mat4(1.0f);
        m_transform = glm::scale(m_transform, glm::vec3(1.0f));

        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void update(double delta) override
    {
        // Update camera.
        update_camera();

        hit_scene();

        update_global_uniforms(m_global_uniforms);

        if (m_debug_gui)
            ui();

        render_g_buffer();
        render_decals();
        render_deferred_shading();

        if (m_debug_gui)
        {
            if (m_is_hit)
                m_debug_draw.frustum(m_projector_view_proj, glm::vec3(1.0f, 0.0f, 0.0f));

            if (m_visualize_projectors)
            {
                for (int i = 0; i < m_decal_instances.size(); i++)
                    m_debug_draw.frustum(m_decal_instances[i].m_projector_view_proj, glm::vec3(0.0f, 1.0f, 0.0f));
            }

            m_debug_draw.render(nullptr, m_width, m_height, m_global_uniforms.view_proj);
        }
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void shutdown() override
    {
        rtcReleaseGeometry(m_embree_triangle_mesh);
        rtcReleaseScene(m_embree_scene);
        rtcReleaseDevice(m_embree_device);

        dw::Mesh::unload(m_mesh);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void window_resized(int width, int height) override
    {
        // Override window resized method to update camera projection.
        m_main_camera->update_projection(60.0f, 0.1f, CAMERA_FAR_PLANE, float(m_width) / float(m_height));
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void key_pressed(int code) override
    {
        // Handle forward movement.
        if (code == GLFW_KEY_W)
            m_heading_speed = m_camera_speed;
        else if (code == GLFW_KEY_S)
            m_heading_speed = -m_camera_speed;

        // Handle sideways movement.
        if (code == GLFW_KEY_A)
            m_sideways_speed = -m_camera_speed;
        else if (code == GLFW_KEY_D)
            m_sideways_speed = m_camera_speed;

        if (code == GLFW_KEY_SPACE)
            m_mouse_look = true;

        if (code == GLFW_KEY_G)
            m_debug_gui = !m_debug_gui;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void key_released(int code) override
    {
        // Handle forward movement.
        if (code == GLFW_KEY_W || code == GLFW_KEY_S)
            m_heading_speed = 0.0f;

        // Handle sideways movement.
        if (code == GLFW_KEY_A || code == GLFW_KEY_D)
            m_sideways_speed = 0.0f;

        if (code == GLFW_KEY_SPACE)
            m_mouse_look = false;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void mouse_pressed(int code) override
    {
        if (code == GLFW_MOUSE_BUTTON_RIGHT && m_is_hit && m_debug_gui)
        {
            DecalInstance instance;

            instance.m_hit_pos             = m_hit_pos;
            instance.m_hit_normal          = m_hit_normal;
            instance.m_hit_distance        = m_hit_distance;
            instance.m_projector_pos       = m_projector_pos;
            instance.m_projector_dir       = m_projector_dir;
            instance.m_projector_view      = m_projector_view;
            instance.m_projector_proj      = m_projector_proj;
            instance.m_projector_view_proj = m_projector_view_proj;
            instance.m_selected_decal      = m_selected_decal;

            m_decal_instances.push_back(instance);
        }

        // Enable mouse look.
        if (code == GLFW_MOUSE_BUTTON_LEFT)
            m_mouse_look = true;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void mouse_released(int code) override
    {
        // Disable mouse look.
        if (code == GLFW_MOUSE_BUTTON_LEFT)
            m_mouse_look = false;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

protected:
    // -----------------------------------------------------------------------------------------------------------------------------------

    dw::AppSettings intial_app_settings() override
    {
        dw::AppSettings settings;

        settings.resizable    = true;
        settings.maximized    = false;
        settings.refresh_rate = 60;
        settings.major_ver    = 4;
        settings.width        = 1920;
        settings.height       = 1080;
        settings.title        = "Deferred Decals (c) 2019 Dihara Wijetunga";

        return settings;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

private:
    // -----------------------------------------------------------------------------------------------------------------------------------

    void hit_scene()
    {
        if (!m_debug_gui)
            return;

        double xpos, ypos;
        glfwGetCursorPos(m_window, &xpos, &ypos);

        glm::vec4 ndc_pos      = glm::vec4((2.0f * float(xpos)) / float(m_width) - 1.0f, 1.0 - (2.0f * float(ypos)) / float(m_height), -1.0f, 1.0f);
        glm::vec4 view_coords  = glm::inverse(m_main_camera->m_projection) * ndc_pos;
        glm::vec4 world_coords = glm::inverse(m_main_camera->m_view) * glm::vec4(view_coords.x, view_coords.y, -1.0f, 0.0f);

        glm::vec3 ray_dir = glm::normalize(glm::vec3(world_coords));

        RTCRayHit rayhit;

        rayhit.ray.dir_x = ray_dir.x;
        rayhit.ray.dir_y = ray_dir.y;
        rayhit.ray.dir_z = ray_dir.z;

        rayhit.ray.org_x = m_main_camera->m_position.x;
        rayhit.ray.org_y = m_main_camera->m_position.y;
        rayhit.ray.org_z = m_main_camera->m_position.z;

        rayhit.ray.tnear     = 0;
        rayhit.ray.tfar      = INFINITY;
        rayhit.ray.mask      = 0;
        rayhit.ray.flags     = 0;
        rayhit.hit.geomID    = RTC_INVALID_GEOMETRY_ID;
        rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

        rtcIntersect1(m_embree_scene, &m_embree_intersect_context, &rayhit);

        if (rayhit.ray.tfar != INFINITY)
        {
            m_hit_pos      = m_main_camera->m_position + ray_dir * rayhit.ray.tfar;
            m_hit_normal   = glm::normalize(glm::vec3(rayhit.hit.Ng_x, rayhit.hit.Ng_y, rayhit.hit.Ng_z));
            m_hit_distance = rayhit.ray.tfar;

            m_projector_pos = m_hit_pos + m_hit_normal * m_projector_outer_depth;
            m_projector_dir = -m_hit_normal;

            glm::mat4 rotate = glm::mat4(1.0f);

            rotate = glm::rotate(rotate, glm::radians(m_projector_rotation), m_projector_dir);

			glm::vec3 default_up   = glm::vec3(0.0f, 0.0f, 1.0f);

			if (m_hit_normal.x > m_hit_normal.y && m_hit_normal.x > m_hit_normal.z)
				default_up = glm::vec3(0.0f, 1.0f, 0.0f);
            else if (m_hit_normal.z > m_hit_normal.y && m_hit_normal.z > m_hit_normal.x)
                default_up = glm::vec3(0.0f, 1.0f, 0.0f);

            glm::vec4 rotated_axis = rotate * glm::vec4(default_up, 0.0f);

            float ratio                = float(m_decal_textures[m_selected_decal]->height()) / float(m_decal_textures[m_selected_decal]->width());
            float proportionate_height = m_projector_size * ratio;

            m_projector_view      = glm::lookAt(m_projector_pos, m_hit_pos, glm::normalize(glm::vec3(rotated_axis) + glm::vec3(0.001f, 0.0f, 0.0f)));
            m_projector_proj      = glm::ortho(-m_projector_size, m_projector_size, -proportionate_height, proportionate_height, 0.1f, m_projector_outer_depth + m_projector_inner_depth);
            m_projector_view_proj = m_projector_proj * m_projector_view;

            m_is_hit = true;
        }
        else
            m_is_hit = false;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void render_g_buffer()
    {
        render_scene(m_g_buffer_fbo.get(), m_g_buffer_program, 0, 0, m_width, m_height, GL_BACK);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void render_decals()
    {
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);

        m_decal_fbo->bind();

        glViewport(0, 0, m_width, m_height);

        // Bind shader program.
        m_decals_program->use();
        m_cube_vao->bind();

        // Bind uniform buffers.
        m_global_ubo->bind_base(0);

        for (int i = 0; i < m_decal_instances.size(); i++)
        {
            m_decals_program->set_uniform("u_InvDecalVP", glm::inverse(m_decal_instances[i].m_projector_view_proj));
            m_decals_program->set_uniform("u_DecalVP", m_decal_instances[i].m_projector_view_proj);

			if (m_decals_program->set_uniform("s_Decal", 0))
                m_decal_textures[m_decal_instances[i].m_selected_decal]->bind(0);

			if (m_decals_program->set_uniform("s_Depth", 1))
				m_depth_rt->bind(1);

            glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
        }

		glDepthMask(GL_TRUE);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void render_deferred_shading()
    {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);

        //m_direct_light_fbo->bind();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glViewport(0, 0, m_width, m_height);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Bind shader program.
        m_deferred_shading_program->use();

        if (m_deferred_shading_program->set_uniform("s_Albedo", 0))
            m_g_buffer_0_rt->bind(0);

        if (m_deferred_shading_program->set_uniform("s_Normals", 1))
            m_g_buffer_1_rt->bind(1);

        if (m_deferred_shading_program->set_uniform("s_Depth", 2))
            m_depth_rt->bind(2);

        // Bind uniform buffers.
        m_global_ubo->bind_base(0);

        // Render fullscreen triangle
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void create_cube()
    {
        const glm::vec4 cube_vertices[] = {
            glm::vec4(-1.0f, -1.0f, 1.0f, 1.0f),  // Far-Bottom-Left
            glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f),   // Far-Top-Left
            glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),    // Far-Top-Right
            glm::vec4(1.0f, -1.0f, 1.0f, 1.0f),   // Far-Bottom-Right
            glm::vec4(-1.0f, -1.0f, -1.0f, 1.0f), // Near-Bottom-Left
            glm::vec4(-1.0f, 1.0f, -1.0f, 1.0f),  // Near-Top-Left
            glm::vec4(1.0f, 1.0f, -1.0f, 1.0f),   // Near-Top-Right
            glm::vec4(1.0f, -1.0f, -1.0f, 1.0f)   // Near-Bottom-Right
        };

        GLushort cube_elements[] = {
            // front
            0,
            1,
            2,
            2,
            3,
            0,
            // right
            1,
            5,
            6,
            6,
            2,
            1,
            // back
            7,
            6,
            5,
            5,
            4,
            7,
            // left
            4,
            0,
            3,
            3,
            7,
            4,
            // bottom
            4,
            5,
            1,
            1,
            0,
            4,
            // top
            3,
            2,
            6,
            6,
            7,
            3
        };

        // Create vertex buffer.
        m_cube_vbo = std::make_unique<dw::VertexBuffer>(GL_STATIC_DRAW, sizeof(cube_vertices), (void*)&cube_vertices[0][0]);

        if (!m_cube_vbo)
            DW_LOG_ERROR("Failed to create Vertex Buffer");

        // Create index buffer.
        m_cube_ibo = std::make_unique<dw::IndexBuffer>(GL_STATIC_DRAW, sizeof(cube_elements), cube_elements);

        if (!m_cube_ibo)
            DW_LOG_ERROR("Failed to create Index Buffer");

        // Declare vertex attributes.
        dw::VertexAttrib attribs[] = { { 4, GL_FLOAT, false, 0 } };

        // Create vertex array.
        m_cube_vao = std::make_unique<dw::VertexArray>(m_cube_vbo.get(), m_cube_ibo.get(), sizeof(float) * 4, 1, attribs);

        if (!m_cube_vao)
            DW_LOG_ERROR("Failed to create Vertex Array");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    bool create_shaders()
    {
        {
            // Create general shaders
            m_g_buffer_vs = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_VERTEX_SHADER, "shader/g_buffer_vs.glsl"));
            m_g_buffer_fs = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_FRAGMENT_SHADER, "shader/g_buffer_fs.glsl"));

            {
                if (!m_g_buffer_vs || !m_g_buffer_fs)
                {
                    DW_LOG_FATAL("Failed to create Shaders");
                    return false;
                }

                // Create general shader program
                dw::Shader* shaders[] = { m_g_buffer_vs.get(), m_g_buffer_fs.get() };
                m_g_buffer_program    = std::make_unique<dw::Program>(2, shaders);

                if (!m_g_buffer_program)
                {
                    DW_LOG_FATAL("Failed to create Shader Program");
                    return false;
                }

                m_g_buffer_program->uniform_block_binding("GlobalUniforms", 0);
            }
        }

        {
            // Create general shaders
            m_decals_vs = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_VERTEX_SHADER, "shader/decals_vs.glsl"));
            m_decals_fs = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_FRAGMENT_SHADER, "shader/decals_fs.glsl"));

            {
                if (!m_decals_vs || !m_decals_fs)
                {
                    DW_LOG_FATAL("Failed to create Shaders");
                    return false;
                }

                // Create general shader program
                dw::Shader* shaders[] = { m_decals_vs.get(), m_decals_fs.get() };
                m_decals_program      = std::make_unique<dw::Program>(2, shaders);

                if (!m_decals_program)
                {
                    DW_LOG_FATAL("Failed to create Shader Program");
                    return false;
                }

                m_decals_program->uniform_block_binding("GlobalUniforms", 0);
            }
        }

        {
            // Create general shaders
            m_fullscreen_triangle_vs = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_VERTEX_SHADER, "shader/fullscreen_triangle_vs.glsl"));
            m_deferred_shading_fs    = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_FRAGMENT_SHADER, "shader/deferred_shading_fs.glsl"));

            {
                if (!m_fullscreen_triangle_vs || !m_deferred_shading_fs)
                {
                    DW_LOG_FATAL("Failed to create Shaders");
                    return false;
                }

                // Create general shader program
                dw::Shader* shaders[]      = { m_fullscreen_triangle_vs.get(), m_deferred_shading_fs.get() };
                m_deferred_shading_program = std::make_unique<dw::Program>(2, shaders);

                if (!m_deferred_shading_program)
                {
                    DW_LOG_FATAL("Failed to create Shader Program");
                    return false;
                }

                m_deferred_shading_program->uniform_block_binding("GlobalUniforms", 0);
            }
        }

        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void create_textures()
    {
        m_g_buffer_0_rt = std::make_unique<dw::Texture2D>(m_width, m_height, 1, 1, 1, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE);
        m_g_buffer_1_rt = std::make_unique<dw::Texture2D>(m_width, m_height, 1, 1, 1, GL_RGB32F, GL_RGB, GL_FLOAT);
        m_depth_rt      = std::make_unique<dw::Texture2D>(m_width, m_height, 1, 1, 1, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT);

        m_g_buffer_0_rt->set_wrapping(GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
        m_g_buffer_1_rt->set_wrapping(GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
        m_depth_rt->set_wrapping(GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void create_framebuffers()
    {
        m_g_buffer_fbo = std::make_unique<dw::Framebuffer>();

        dw::Texture* gbuffer_rts[] = { m_g_buffer_0_rt.get(), m_g_buffer_1_rt.get() };
        m_g_buffer_fbo->attach_multiple_render_targets(2, gbuffer_rts);
        m_g_buffer_fbo->attach_depth_stencil_target(m_depth_rt.get(), 0, 0);

		m_decal_fbo = std::make_unique<dw::Framebuffer>();

        dw::Texture* decals_rts[] = { m_g_buffer_0_rt.get() };
        m_decal_fbo->attach_multiple_render_targets(1, decals_rts);
        m_decal_fbo->attach_depth_stencil_target(m_depth_rt.get(), 0, 0);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    bool create_uniform_buffer()
    {
        // Create uniform buffer for global data
        m_global_ubo = std::make_unique<dw::UniformBuffer>(GL_DYNAMIC_DRAW, sizeof(GlobalUniforms));

        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void ui()
    {
        ImGui::DragFloat("Decal Rotation", &m_projector_rotation, 1.0f, -180.0f, 180.0f);
        ImGui::DragFloat("Decal Size", &m_projector_size, 5.0f, 10.0f, 200.0f);
        ImGui::DragFloat("Decal Extents Outer", &m_projector_outer_depth, 1.0f, 2.0f, 50.0f);
        ImGui::DragFloat("Decal Extents Inner", &m_projector_inner_depth, 1.0f, 2.0f, 50.0f);
        ImGui::Checkbox("Visualize Projectors", &m_visualize_projectors);

        const char* listbox_items[] = { "OpenGL", "Vulkan", "DirectX", "Metal" };
        ImGui::ListBox("Selected Decal", &m_selected_decal, listbox_items, IM_ARRAYSIZE(listbox_items), 4);

        if (!GLAD_GL_NV_conservative_raster && !GLAD_GL_INTEL_conservative_rasterization)
        {
            ImGui::Separator();
            ImGui::Text("Note: Conservative Rasterization not supported on this GPU.");
        }
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    bool load_scene()
    {
        m_mesh = dw::Mesh::load("mesh/sponza.obj");

        if (!m_mesh)
        {
            DW_LOG_FATAL("Failed to load mesh!");
            return false;
        }

        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    bool load_decals()
    {
        m_decal_textures.resize(4);
        m_decal_textures[0] = std::unique_ptr<dw::Texture2D>(dw::Texture2D::create_from_files("texture/opengl.png", false, true));
        m_decal_textures[0]->set_min_filter(GL_LINEAR_MIPMAP_LINEAR);
        m_decal_textures[0]->set_mag_filter(GL_LINEAR);

        m_decal_textures[1] = std::unique_ptr<dw::Texture2D>(dw::Texture2D::create_from_files("texture/vulkan.png", false, true));
        m_decal_textures[1]->set_min_filter(GL_LINEAR_MIPMAP_LINEAR);
        m_decal_textures[1]->set_mag_filter(GL_LINEAR);

        m_decal_textures[2] = std::unique_ptr<dw::Texture2D>(dw::Texture2D::create_from_files("texture/directx.png", false, true));
        m_decal_textures[2]->set_min_filter(GL_LINEAR_MIPMAP_LINEAR);
        m_decal_textures[2]->set_mag_filter(GL_LINEAR);

        m_decal_textures[3] = std::unique_ptr<dw::Texture2D>(dw::Texture2D::create_from_files("texture/metal.png", false, true));
        m_decal_textures[3]->set_min_filter(GL_LINEAR_MIPMAP_LINEAR);
        m_decal_textures[3]->set_mag_filter(GL_LINEAR);

        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    bool initialize_embree()
    {
        m_embree_device = rtcNewDevice(nullptr);

        RTCError embree_error = rtcGetDeviceError(m_embree_device);

        if (embree_error == RTC_ERROR_UNSUPPORTED_CPU)
            throw std::runtime_error("Your CPU does not meet the minimum requirements for embree");
        else if (embree_error != RTC_ERROR_NONE)
            throw std::runtime_error("Failed to initialize embree!");

        m_embree_scene = rtcNewScene(m_embree_device);

        m_embree_triangle_mesh = rtcNewGeometry(m_embree_device, RTC_GEOMETRY_TYPE_TRIANGLE);

        std::vector<glm::vec3> vertices(m_mesh->vertex_count());
        std::vector<uint32_t>  indices(m_mesh->index_count());
        uint32_t               idx        = 0;
        dw::Vertex*            vertex_ptr = m_mesh->vertices();
        uint32_t*              index_ptr  = m_mesh->indices();

        for (int i = 0; i < m_mesh->vertex_count(); i++)
            vertices[i] = vertex_ptr[i].position;

        for (int i = 0; i < m_mesh->sub_mesh_count(); i++)
        {
            dw::SubMesh& submesh = m_mesh->sub_meshes()[i];

            for (int j = submesh.base_index; j < (submesh.base_index + submesh.index_count); j++)
                indices[idx++] = submesh.base_vertex + index_ptr[j];
        }

        void* data = rtcSetNewGeometryBuffer(m_embree_triangle_mesh, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(glm::vec3), m_mesh->vertex_count());
        memcpy(data, vertices.data(), vertices.size() * sizeof(glm::vec3));

        data = rtcSetNewGeometryBuffer(m_embree_triangle_mesh, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(uint32_t), m_mesh->index_count() / 3);
        memcpy(data, indices.data(), indices.size() * sizeof(uint32_t));

        rtcCommitGeometry(m_embree_triangle_mesh);
        rtcAttachGeometry(m_embree_scene, m_embree_triangle_mesh);
        rtcCommitScene(m_embree_scene);

        rtcInitIntersectContext(&m_embree_intersect_context);

        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void create_camera()
    {
        m_main_camera = std::make_unique<dw::Camera>(60.0f, 0.1f, CAMERA_FAR_PLANE, float(m_width) / float(m_height), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0, 0.0f));
        m_main_camera->update();
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void render_mesh(dw::Mesh* mesh, glm::mat4 model, std::unique_ptr<dw::Program>& program)
    {
        program->set_uniform("u_Model", model);

        // Bind vertex array.
        mesh->mesh_vertex_array()->bind();

        dw::SubMesh* submeshes = mesh->sub_meshes();

        for (uint32_t i = 0; i < mesh->sub_mesh_count(); i++)
        {
            dw::SubMesh& submesh = submeshes[i];

			if (submesh.mat->texture(0))
			{
				if (program->set_uniform("s_Albedo", 0))
					submesh.mat->texture(0)->bind(0);
			}

            // Issue draw call.
            glDrawElementsBaseVertex(GL_TRIANGLES, submesh.index_count, GL_UNSIGNED_INT, (void*)(sizeof(unsigned int) * submesh.base_index), submesh.base_vertex);
        }
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void render_scene(dw::Framebuffer* fbo, std::unique_ptr<dw::Program>& program, int x, int y, int w, int h, GLenum cull_face, bool clear = true)
    {
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);

        if (cull_face == GL_NONE)
            glDisable(GL_CULL_FACE);
        else
        {
            glEnable(GL_CULL_FACE);
            glCullFace(cull_face);
        }

        if (fbo)
            fbo->bind();
        else
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glViewport(x, y, w, h);

        if (clear)
        {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClearDepth(1.0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }

        // Bind shader program.
        program->use();

        // Bind uniform buffers.
        m_global_ubo->bind_base(0);

        // Draw scene.
        render_mesh(m_mesh, m_transform, program);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void update_global_uniforms(const GlobalUniforms& global)
    {
        void* ptr = m_global_ubo->map(GL_WRITE_ONLY);
        memcpy(ptr, &global, sizeof(GlobalUniforms));
        m_global_ubo->unmap();
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void update_transforms(dw::Camera* camera)
    {
        // Update camera matrices.
        m_global_uniforms.view_proj = camera->m_projection * camera->m_view;
        m_global_uniforms.inv_view_proj = glm::inverse(m_global_uniforms.view_proj);
        m_global_uniforms.cam_pos   = glm::vec4(camera->m_position, 0.0f);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void update_camera()
    {
        dw::Camera* current = m_main_camera.get();

        float forward_delta = m_heading_speed * m_delta;
        float right_delta   = m_sideways_speed * m_delta;

        current->set_translation_delta(current->m_forward, forward_delta);
        current->set_translation_delta(current->m_right, right_delta);

        m_camera_x = m_mouse_delta_x * m_camera_sensitivity;
        m_camera_y = m_mouse_delta_y * m_camera_sensitivity;

        if (m_mouse_look)
        {
            // Activate Mouse Look
            current->set_rotatation_delta(glm::vec3((float)(m_camera_y),
                                                    (float)(m_camera_x),
                                                    (float)(0.0f)));
        }
        else
        {
            current->set_rotatation_delta(glm::vec3((float)(0),
                                                    (float)(0),
                                                    (float)(0)));
        }

        current->update();
        update_transforms(current);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

private:
    // General GPU resources.
    std::unique_ptr<dw::Shader> m_g_buffer_vs;
    std::unique_ptr<dw::Shader> m_g_buffer_fs;
    std::unique_ptr<dw::Shader> m_decals_vs;
    std::unique_ptr<dw::Shader> m_decals_fs;
    std::unique_ptr<dw::Shader> m_fullscreen_triangle_vs;
    std::unique_ptr<dw::Shader> m_deferred_shading_fs;

    std::unique_ptr<dw::Program> m_g_buffer_program;
    std::unique_ptr<dw::Program> m_decals_program;
    std::unique_ptr<dw::Program> m_deferred_shading_program;

    std::unique_ptr<dw::Texture2D> m_g_buffer_0_rt;
    std::unique_ptr<dw::Texture2D> m_g_buffer_1_rt;
    std::unique_ptr<dw::Texture2D> m_depth_rt;

    std::unique_ptr<dw::Framebuffer> m_g_buffer_fbo;
    std::unique_ptr<dw::Framebuffer> m_decal_fbo;

    std::vector<std::unique_ptr<dw::Texture2D>> m_decal_textures;

    std::unique_ptr<dw::UniformBuffer> m_global_ubo;

    std::unique_ptr<dw::VertexBuffer> m_cube_vbo;
    std::unique_ptr<dw::IndexBuffer>  m_cube_ibo;
    std::unique_ptr<dw::VertexArray>  m_cube_vao;

    // Camera.
    std::unique_ptr<dw::Camera> m_main_camera;

    GlobalUniforms m_global_uniforms;

    // Scene
    dw::Mesh* m_mesh;
    glm::mat4 m_transform;

    // Camera controls.
    bool  m_mouse_look         = false;
    float m_heading_speed      = 0.0f;
    float m_sideways_speed     = 0.0f;
    float m_camera_sensitivity = 0.05f;
    float m_camera_speed       = 0.2f;
    bool  m_debug_gui          = true;

    // Embree structure
    RTCDevice           m_embree_device        = nullptr;
    RTCScene            m_embree_scene         = nullptr;
    RTCGeometry         m_embree_triangle_mesh = nullptr;
    RTCIntersectContext m_embree_intersect_context;

    // Last hit
    glm::vec3 m_hit_pos;
    glm::vec3 m_hit_normal;
    float     m_hit_distance = INFINITY;

    // Projector
    glm::vec3 m_projector_pos;
    glm::vec3 m_projector_dir;
    glm::mat4 m_projector_view;
    glm::mat4 m_projector_proj;
    glm::mat4 m_projector_view_proj;

    float m_projector_size        = 10.0f;
    float m_projector_rotation    = 0.0f;
    float m_projector_outer_depth = 10.0f;
    float m_projector_inner_depth = 10.0f;

    // Debug
    int32_t m_selected_decal = 0;

    std::vector<DecalInstance> m_decal_instances;

    // Camera orientation.
    float m_camera_x;
    float m_camera_y;
    bool  m_is_hit               = false;
    bool  m_visualize_projectors = false;
};

DW_DECLARE_MAIN(DeferredDecals)