# kyty_launcher: the settings window for Linux (launcher-windows' counterpart). SDL2 (its static library, as the
# emulator links it) draws the window and Dear ImGui its controls, so the program needs nothing installed beyond what the
# emulator needs; it starts the game through run_ds.sh.
add_executable(kyty_launcher
	main.cpp
	ui.cpp
	commands.cpp
	game.cpp
	platform.cpp
	settings.cpp
	strings.cpp
	stb_image.cpp
	"${KYTY_THIRD_PARTY_DIR}/imgui/backends/imgui_impl_sdl2.cpp"
	"${KYTY_THIRD_PARTY_DIR}/imgui/backends/imgui_impl_sdlrenderer2.cpp"
	"${KYTY_THIRD_PARTY_DIR}/imgui/misc/cpp/imgui_stdlib.cpp"
)
target_include_directories(kyty_launcher PRIVATE
	"${KYTY_THIRD_PARTY_DIR}/SDL2/include"
	"${KYTY_THIRD_PARTY_DIR}/imgui"
	"${KYTY_THIRD_PARTY_DIR}/imgui/backends"
	"${KYTY_THIRD_PARTY_DIR}/stb"
)
find_package(Threads REQUIRED)
target_link_libraries(kyty_launcher PRIVATE SDL2-static imgui nlohmann_json::nlohmann_json Vulkan::Headers Threads::Threads
	${CMAKE_DL_LIBS})
set_target_properties(kyty_launcher PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
# (No symbols in a release: a third of the size.)
target_link_options(kyty_launcher PRIVATE $<$<CONFIG:Release>:-s>)
