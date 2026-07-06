// Веха 1: проверка захвата Daheng в C++.
// Открывает камеру, снимает N секунд, печатает FPS/разрешение/формат,
// самописным дебайером конвертит последний кадр и сохраняет capture.ppm.
//
// Запуск:  ./camctl [serial] [seconds]
#include "camera/daheng_camera.hpp"
#include "core/debayer.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace camctl;

static void save_ppm(const Frame& rgb, const char* path) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << rgb.width << " " << rgb.height << "\n255\n";
    f.write(reinterpret_cast<const char*>(rgb.data.data()),
            static_cast<std::streamsize>(rgb.data.size()));
}

int main(int argc, char** argv) {
    const std::string serial = (argc > 1) ? argv[1] : "";
    const int seconds = (argc > 2) ? std::atoi(argv[2]) : 3;

    DahengCamera cam(serial);
    if (!cam.open()) {
        std::fprintf(stderr, "open() не удался\n");
        return 1;
    }
    std::printf("Открыта камера: %s\n", cam.info().c_str());

    if (!cam.start()) {
        std::fprintf(stderr, "start() не удался\n");
        return 1;
    }

    const auto t0 = std::chrono::steady_clock::now();
    int count = 0, misses = 0;
    Frame f, last;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        if (cam.grab(f, 500)) { ++count; last = f; }
        else                  { ++misses; }
    }
    cam.stop();

    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("Кадров: %d за %.1f c (%.1f fps), таймаутов/промахов: %d\n",
                count, dt, dt > 0 ? count / dt : 0.0, misses);

    if (last.valid()) {
        std::printf("Последний кадр: %dx%d, формат=%d\n", last.width, last.height,
                    static_cast<int>(last.format));
        Frame rgb = debayer_to_rgb(last);
        if (rgb.valid()) {
            save_ppm(rgb, "capture.ppm");
            std::printf("Сохранил capture.ppm (%dx%d)\n", rgb.width, rgb.height);
        }
    }

    cam.close();
    return count > 0 ? 0 : 2;
}
