serenity_component(
    Snake
    RECOMMENDED
    TARGETS Snake
)

set(SOURCES
    main.cpp
    SnakeGame.cpp
)

serenity_app(Snake ICON app-snake)
target_link_libraries(Snake PRIVATE LibCore LibGfx LibGUI LibConfig LibMain LibDesktop)
