add_rules("mode.debug", "mode.release")

-- Set project
set_project("linscribe")

-- Set languages
set_languages("c++17")

-- Set warnings
set_warnings("all", "error")

-- System dependencies
add_requires("gtk3", {system = true})

package("libpulse")
    set_homepage("https://www.freedesktop.org/wiki/Software/PulseAudio/")
    add_extsources("pkgconfig::libpulse")
    on_fetch(function (package, opt)
        if opt.system then
            return package:find_package("pkgconfig::libpulse")
        end
    end)
package_end()
add_requires("libpulse", {system = true})

package("libpulse-mainloop-glib")
    set_homepage("https://www.freedesktop.org/wiki/Software/PulseAudio/")
    add_extsources("pkgconfig::libpulse-mainloop-glib")
    on_fetch(function (package, opt)
        if opt.system then
            return package:find_package("pkgconfig::libpulse-mainloop-glib")
        end
    end)
package_end()
add_requires("libpulse-mainloop-glib", {system = true})

package("ayatana-appindicator3")
    set_homepage("https://github.com/AyatanaIndicators/libayatana-appindicator")
    add_extsources("pkgconfig::ayatana-appindicator3-0.1")
    on_fetch(function (package, opt)
        if opt.system then
            return package:find_package("pkgconfig::ayatana-appindicator3-0.1")
        end
    end)
package_end()
add_requires("ayatana-appindicator3", {system = true})

package("libsoup-3.0")
    set_homepage("https://libsoup.org/")
    add_extsources("pkgconfig::libsoup-3.0")
    on_fetch(function (package, opt)
        if opt.system then
            return package:find_package("pkgconfig::libsoup-3.0")
        end
    end)
package_end()
add_requires("libsoup-3.0", {system = true})

package("json-glib")
    set_homepage("https://gnome.pages.gitlab.gnome.org/json-glib/")
    add_extsources("pkgconfig::json-glib-1.0")
    on_fetch(function (package, opt)
        if opt.system then
            return package:find_package("pkgconfig::json-glib-1.0")
        end
    end)
package_end()
add_requires("json-glib", {system = true})

package("keybinder-3.0")
    set_homepage("https://github.com/kupferlaunux/keybinder")
    add_extsources("pkgconfig::keybinder-3.0")
    on_fetch(function (package, opt)
        if opt.system then
            return package:find_package("pkgconfig::keybinder-3.0")
        end
    end)
package_end()
add_requires("keybinder-3.0", {system = true})

package("libxdo")
    set_homepage("https://github.com/jordansissel/xdotool")
    on_fetch(function (package)
        return {links = "xdo"}
    end)
package_end()
add_requires("libxdo", {system = true})

-- Define target
target("linscribe")
    set_kind("binary")
    add_files("src/*.cpp")
    add_packages("gtk3", "ayatana-appindicator3", "libpulse", "libpulse-mainloop-glib", "libsoup-3.0", "json-glib", "keybinder-3.0", "libxdo")

    -- Set optimization
    if is_mode("release") then
        set_optimize("fastest")
    elseif is_mode("debug") then
        set_optimize("none")
    end
