-- add_rules("mode.debug", "mode.release")
--
-- target("REBEL")
-- 	set_kind("binary")
-- 	add_files("src/*.cpp")
-- 	set_optimize('smallest')
-- 	add_links('ncurses')
-- 	add_links('miniupnpc')
-- 	add_syslinks("pthread")

add_rules("mode.debug", "mode.release")

-- Сообщаем xmake, что нам нужны эти пакеты. 
-- configs = {shared = false} заставляет xmake собрать их как статические (.a) библиотеки.
add_requires("ncurses", {configs = {shared = false}})
add_requires("miniupnpc", {configs = {shared = false}})

target("REBEL")
    set_kind("binary")

    -- Убедись, что твой main.cpp лежит в папке src/, 
    -- либо поменяй путь на add_files("*.cpp"), если он лежит в корне.
    add_files("src/*.cpp")

    set_optimize("smallest")

    -- Подключаем собранные пакеты к нашему таргету
    add_packages("ncurses", "miniupnpc")

    add_syslinks("pthread")

    -- Если ты собираешь под Linux и хочешь, чтобы бинарник был максимально независимым,
    -- стоит также статически прилинковать стандартные библиотеки C++ и GCC.
    if is_plat("linux") then
        add_ldflags("-static-libstdc++", "-static-libgcc")
    end

