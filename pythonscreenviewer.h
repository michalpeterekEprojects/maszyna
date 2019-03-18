#include "renderer.h"
#include "PyInt.h"
#include <GLFW/glfw3.h>

class python_screen_viewer
{
	struct window_state {
		GLFWwindow *window = nullptr;
		glm::ivec2 size;

		glm::vec2 offset;
		glm::vec2 scale;

		std::unique_ptr<gl::ubo> ubo;
		std::unique_ptr<gl::program> shader;

		window_state() = default;
		~window_state();
	};

	std::vector<std::unique_ptr<window_state>> m_windows;

	std::shared_ptr<python_rt> m_rt;
	std::shared_ptr<std::thread> m_renderthread;
	gl::scene_ubs m_ubs;

	std::atomic_bool m_exit = false;

	void threadfunc();

public:
	python_screen_viewer(std::shared_ptr<python_rt> rt, std::string name);
	~python_screen_viewer();

	void notify_window_size(GLFWwindow *window, int w, int h);
};
