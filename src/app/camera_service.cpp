// Веха 3: сервис одной камеры. Захват -> дебайер -> даунскейл -> I420 -> NVENC H.264 -> RTP/RTSP,
// плюс управляющий канал TCP+JSON (живая смена битрейта/разрешения/fps/экспозиции/усиления/децимации).
//
// Все мутации устройства и энкодера выполняются В ПОТОКЕ ЦИКЛА (очередь задач) — без гонок с захватом.
//
// Запуск:  ./camera_service [serial] [rtsp_port] [ctrl_port] [fps] [maxside]
#include "camera/daheng_camera.hpp"
#include "core/debayer.hpp"
#include "core/rgb_to_i420.hpp"
#include "encode/h264_encoder.hpp"
#include "stream/rtsp_server.hpp"
#include "control/control_server.hpp"
#include "control/param_registry.hpp"
#include "control/json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace camctl;

static std::atomic<bool> g_run{true};
static void on_sig(int) { g_run = false; }

// CLOCK_MONOTONIC (не CLOCK_REALTIME!): реальные часы Jetson шагает NTP, из-за чего
// кросс-машинный замер задержки ломается. Монотонные часы стабильны, а произвольная
// точка отсчёта сокращается при обмене смещением по control-каналу.
static int64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// Собрать SEI-NAL (Annex-B) с меткой захвата: user_data_unregistered (payloadType=5),
// 16-байтный UUID (ASCII, без нулей — легко искать во вьюере) + 8 байт ts (big-endian, ns).
// С emulation-prevention (вставка 0x03 после 00 00), как требует H.264.
static std::vector<uint8_t> build_sei_annexb(int64_t ts_ns) {
    static const uint8_t uuid[16] = {'C','A','M','C','T','L','-','T','S','-','0','0','0','1','!','!'};
    std::vector<uint8_t> rbsp;
    rbsp.push_back(0x05);                 // payloadType = user_data_unregistered
    rbsp.push_back(24);                   // payloadSize = 16 (uuid) + 8 (ts)
    for (int i = 0; i < 16; ++i) rbsp.push_back(uuid[i]);
    for (int i = 7; i >= 0; --i) rbsp.push_back(static_cast<uint8_t>((ts_ns >> (i * 8)) & 0xFF));
    rbsp.push_back(0x80);                 // rbsp_trailing_bits

    std::vector<uint8_t> out = {0, 0, 0, 1, 0x06};  // старт-код + NAL header (type 6 = SEI)
    int zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) { out.push_back(0x03); zeros = 0; }
        out.push_back(b);
        zeros = (b == 0) ? zeros + 1 : 0;
    }
    return out;
}

static void downscale_rgb24(const uint8_t* src, int sw, int sh,
                            uint8_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        const int sy = static_cast<int>((static_cast<int64_t>(y) * sh) / dh);
        const uint8_t* srow = src + static_cast<size_t>(sy) * sw * 3;
        uint8_t* drow = dst + static_cast<size_t>(y) * dw * 3;
        for (int x = 0; x < dw; ++x) {
            const int sx = static_cast<int>((static_cast<int64_t>(x) * sw) / dw);
            drow[3 * x]     = srow[3 * sx];
            drow[3 * x + 1] = srow[3 * sx + 1];
            drow[3 * x + 2] = srow[3 * sx + 2];
        }
    }
}

class CameraService {
public:
    CameraService(std::string serial, int rtsp_port, int ctrl_port, int fps, int maxside,
                  std::string startup_preset = "")
        : cam_(std::move(serial)), rtsp_port_(rtsp_port), ctrl_port_(ctrl_port),
          maxside_(maxside), startup_preset_(std::move(startup_preset)), enc_fps_(fps) {}

    bool init() {
        if (!cam_.open())  { std::fprintf(stderr, "open() не удался\n"); return false; }
        std::printf("Камера: %s\n", cam_.info().c_str());
        if (!cam_.start()) { std::fprintf(stderr, "start() не удался\n"); return false; }

        Frame raw;
        for (int i = 0; i < 50 && !raw.valid(); ++i) cam_.grab(raw, 500);
        if (!raw.valid()) { std::fprintf(stderr, "нет кадров с камеры\n"); return false; }
        src_w_ = raw.width; src_h_ = raw.height;

        // Начальные размеры энкодера: ограничиваем большую сторону.
        enc_w_ = src_w_; enc_h_ = src_h_;
        if (maxside_ > 0 && (src_w_ > maxside_ || src_h_ > maxside_)) {
            const double s = static_cast<double>(maxside_) / std::max(src_w_, src_h_);
            enc_w_ = static_cast<int>(src_w_ * s);
            enc_h_ = static_cast<int>(src_h_ * s);
        }
        enc_w_ &= ~1; enc_h_ &= ~1;
        enc_idr_ = enc_fps_;
        enc_bitrate_ = 6000000;

        // Прочитать реальные диапазоны экспозиции/усиления (best-effort).
        cam_.get_float_range("ExposureTime", exp_lo_, exp_hi_);
        cam_.get_float_range("Gain", gain_lo_, gain_hi_);
        cam_.get_float("ExposureTime", exposure_us_);
        cam_.get_float("Gain", gain_);

        rtsp_ = std::make_unique<RtspServer>(rtsp_port_, enc_fps_);
        if (!rtsp_->start()) return false;
        rtsp_->set_keyframe_requester([this] { if (enc_) enc_->request_keyframe(); });

        if (!create_encoder()) return false;

        build_registry();
        ctrl_ = std::make_unique<ControlServer>(
            ctrl_port_, &reg_,
            [this] { return stats(); },
            [this](const std::string& n, std::string& e) { return load_preset(n, e); });
        ctrl_->set_save_handler([this](const std::string& n, std::string& e) { return save_preset(n, e); });
        ctrl_->set_list_handler([this] { return list_presets(); });
        if (!ctrl_->start()) return false;

        // Стартовый пресет (значения по умолчанию по имени) — если задан.
        if (!startup_preset_.empty()) {
            std::string e;
            if (!load_preset(startup_preset_, e))
                std::fprintf(stderr, "стартовый пресет '%s': %s\n", startup_preset_.c_str(), e.c_str());
        }

        std::printf("Готово. rtsp://<host>:%d/cam0  |  control tcp://<host>:%d  |  пресеты: %s/\n",
                    rtsp_port_, ctrl_port_, presets_dir_.c_str());
        std::fflush(stdout);
        return true;
    }

    void run() {
        std::vector<uint8_t> scaled, i420;
        auto t_fps = std::chrono::steady_clock::now();
        int frames_in_window = 0;

        while (g_run) {
            drain_tasks();
            // Пересоздание энкодера (коалесцированное) — в потоке цикла, без гонок с encodeFrame.
            { bool need; { std::lock_guard<std::mutex> lk(mu_); need = enc_dirty_; enc_dirty_ = false; }
              if (need) create_encoder(); }

            Frame f;
            if (!cam_.grab(f, 500)) continue;
            const int64_t cap_ns = mono_ns();   // метка захвата (host, CLOCK_MONOTONIC)
            Frame rgb = debayer_to_rgb(f);
            if (!rgb.valid()) continue;

            int ew, eh;
            { std::lock_guard<std::mutex> lk(mu_); ew = enc_w_; eh = enc_h_; }

            scaled.resize(static_cast<size_t>(ew) * eh * 3);
            i420.resize(static_cast<size_t>(ew) * eh * 3 / 2);
            downscale_rgb24(rgb.data.data(), rgb.width, rgb.height, scaled.data(), ew, eh);
            rgb24_to_i420(scaled.data(), ew, eh, i420.data());
            if (enc_) {
                { std::lock_guard<std::mutex> lk(ts_mu_);
                  ts_fifo_.push_back(cap_ns);
                  while (ts_fifo_.size() > 10) ts_fifo_.pop_front(); }  // защита от рассинхрона
                enc_->encodeFrame(i420.data(), i420.size());
            }

            // Замер FPS раз в ~1с.
            ++frames_in_window;
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - t_fps).count();
            if (dt >= 1.0) {
                fps_meas_.store(frames_in_window / dt);
                frames_in_window = 0;
                t_fps = now;
            }
        }
    }

    void shutdown() {
        if (ctrl_) ctrl_->stop();
        if (rtsp_) rtsp_->stop();
        if (enc_)  enc_->finish();
        cam_.stop();
        cam_.close();
    }

private:
    // --- пересоздание энкодера под текущий enc_* ---
    // ВАЖНО: NVENC — один инстанс; старый уничтожаем ПЕРВЫМ (иначе "resource busy"),
    // и только чистым EOS-остановом (иначе DQ-поток виснет в ioctl).
    bool create_encoder() {
        int w, h, fps, br;
        { std::lock_guard<std::mutex> lk(mu_);
          w = enc_w_; h = enc_h_; fps = enc_fps_; br = enc_bitrate_; }
        enc_.reset();  // чистый останов + освобождение старого до создания нового
        { std::lock_guard<std::mutex> lk(ts_mu_); ts_fifo_.clear(); }  // метки старого энкодера не валидны
        auto e = std::make_unique<H264Encoder>();
        if (!e->init(w, h, fps, br)) { std::fprintf(stderr, "encoder init %dx%d failed\n", w, h); return false; }
        // on_nal: достаём метку захвата этого кадра (FIFO) и вставляем SEI перед access unit.
        e->on_nal([this](const uint8_t* d, size_t n, bool key) {
            int64_t cap;
            { std::lock_guard<std::mutex> lk(ts_mu_);
              if (!ts_fifo_.empty()) { cap = ts_fifo_.front(); ts_fifo_.pop_front(); }
              else cap = mono_ns(); }
            std::vector<uint8_t> sei = build_sei_annexb(cap);
            sei.insert(sei.end(), d, d + n);          // SEI + access unit одним буфером
            rtsp_->push_au(sei.data(), sei.size(), key);
        });
        e->request_keyframe();
        enc_ = std::move(e);
        std::printf("Энкодер пересоздан: %dx%d @ %d fps, %d bps\n", w, h, fps, br);
        std::fflush(stdout);
        return true;
    }

    // --- задачи, исполняемые в потоке цикла ---
    void push_task(std::function<void()> t) {
        std::lock_guard<std::mutex> lk(task_mu_);
        tasks_.push_back(std::move(t));
    }
    void drain_tasks() {
        std::vector<std::function<void()>> local;
        { std::lock_guard<std::mutex> lk(task_mu_); local.swap(tasks_); }
        for (auto& t : local) t();
    }

    void apply_camera_runtime() {
        double exp, gn; bool ae;
        { std::lock_guard<std::mutex> lk(mu_); exp = exposure_us_; gn = gain_; ae = auto_exp_; }
        if (ae) {
            cam_.set_enum("ExposureAuto", "Continuous");
        } else {
            cam_.set_enum("ExposureAuto", "Off");
            cam_.set_float("ExposureTime", exp);
        }
        cam_.set_enum("GainAuto", "Off");
        cam_.set_float("Gain", gn);
    }

    void apply_decimation() {
        int d;
        { std::lock_guard<std::mutex> lk(mu_); d = decim_; }
        cam_.stop();
        cam_.set_int("DecimationHorizontal", d);
        cam_.set_int("DecimationVertical", d);
        int64_t w = 0, h = 0;
        cam_.get_int("Width", w);
        cam_.get_int("Height", h);
        { std::lock_guard<std::mutex> lk(mu_); src_w_ = static_cast<int>(w); src_h_ = static_cast<int>(h); }
        cam_.start();
        std::printf("Децимация=%d -> сенсор %lldx%lld\n", d, (long long)w, (long long)h);
    }

    // --- реестр параметров ---
    void build_registry() {
        using V = json::Value;

        // Энкодер: битрейт — рантайм.
        { ParamInfo pi; pi.name = "bitrate"; pi.type = "int"; pi.unit = "bps";
          pi.has_range = true; pi.min = 100000; pi.max = 50000000;
          reg_.add(pi,
            [this] { std::lock_guard<std::mutex> lk(mu_); return V::N(enc_bitrate_); },
            [this](const V& v, std::string&) {
                { std::lock_guard<std::mutex> lk(mu_); enc_bitrate_ = (int)v.as_num(); }
                int br = enc_bitrate_;
                push_task([this, br] { if (enc_) enc_->set_bitrate(br); });
                return true; }); }

        // Энкодер: ширина/высота/fps — пересоздание.
        auto add_enc_recreate = [this](const char* name, int* field, double lo, double hi) {
            ParamInfo pi; pi.name = name; pi.type = "int"; pi.has_range = true; pi.min = lo; pi.max = hi;
            reg_.add(pi,
              [this, field] { std::lock_guard<std::mutex> lk(mu_); return json::Value::N(*field); },
              [this, field](const json::Value& v, std::string&) {
                  // Коалесцируем: width+height+fps за один тик цикла = одно пересоздание.
                  std::lock_guard<std::mutex> lk(mu_);
                  *field = ((int)v.as_num()) & ~1; if (*field < 2) *field = 2;
                  enc_dirty_ = true;
                  return true; });
        };
        add_enc_recreate("enc_width",  &enc_w_,  16, 4096);
        add_enc_recreate("enc_height", &enc_h_,  16, 4096);
        add_enc_recreate("fps",        &enc_fps_, 1,  60);

        // Камера: экспозиция (µs) — рантайм.
        { ParamInfo pi; pi.name = "exposure_us"; pi.type = "float"; pi.unit = "us";
          if (exp_hi_ > exp_lo_) { pi.has_range = true; pi.min = exp_lo_; pi.max = exp_hi_; }
          reg_.add(pi,
            [this] { std::lock_guard<std::mutex> lk(mu_); return V::N(exposure_us_); },
            [this](const V& v, std::string&) {
                { std::lock_guard<std::mutex> lk(mu_); exposure_us_ = v.as_num(); auto_exp_ = false; }
                push_task([this] { apply_camera_runtime(); });
                return true; }); }

        // Камера: авто-экспозиция вкл/выкл.
        { ParamInfo pi; pi.name = "auto_exposure"; pi.type = "bool";
          reg_.add(pi,
            [this] { std::lock_guard<std::mutex> lk(mu_); return V::B(auto_exp_); },
            [this](const V& v, std::string&) {
                { std::lock_guard<std::mutex> lk(mu_); auto_exp_ = v.as_bool(); }
                push_task([this] { apply_camera_runtime(); });
                return true; }); }

        // Камера: усиление.
        { ParamInfo pi; pi.name = "gain"; pi.type = "float"; pi.unit = "dB";
          if (gain_hi_ > gain_lo_) { pi.has_range = true; pi.min = gain_lo_; pi.max = gain_hi_; }
          reg_.add(pi,
            [this] { std::lock_guard<std::mutex> lk(mu_); return V::N(gain_); },
            [this](const V& v, std::string&) {
                { std::lock_guard<std::mutex> lk(mu_); gain_ = v.as_num(); }
                push_task([this] { apply_camera_runtime(); });
                return true; }); }

        // Камера: децимация (1/2/4) — рестарт стрима.
        { ParamInfo pi; pi.name = "decimation"; pi.type = "int"; pi.has_range = true; pi.min = 1; pi.max = 8;
          reg_.add(pi,
            [this] { std::lock_guard<std::mutex> lk(mu_); return V::N(decim_); },
            [this](const V& v, std::string&) {
                { std::lock_guard<std::mutex> lk(mu_); decim_ = std::max(1, (int)v.as_num()); }
                push_task([this] { apply_decimation(); });
                return true; }); }
    }

    json::Value stats() {
        std::lock_guard<std::mutex> lk(mu_);
        json::Value o = json::Value::O();
        o.set("fps", json::Value::N(fps_meas_.load()));
        o.set("enc_width", json::Value::N(enc_w_));
        o.set("enc_height", json::Value::N(enc_h_));
        o.set("enc_fps", json::Value::N(enc_fps_));
        o.set("bitrate", json::Value::N(enc_bitrate_));
        o.set("src_width", json::Value::N(src_w_));
        o.set("src_height", json::Value::N(src_h_));
        o.set("decimation", json::Value::N(decim_));
        return o;
    }

    // --- пресеты (JSON-файлы presets/<name>.json) ---
    // Файл — плоский объект {имя_ручки: значение}. Применяется через тот же путь reg_.set,
    // поэтому reconfig/задачи отрабатывают штатно.
    bool load_preset(const std::string& name, std::string& err) {
        const std::string path = presets_dir_ + "/" + name + ".json";
        std::ifstream f(path, std::ios::binary);
        if (!f) { err = "пресет не найден: " + path; return false; }
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        json::Value v;
        if (!json::parse(text, v) || v.type != json::Value::Obj) { err = "битый JSON пресета"; return false; }

        int applied = 0;
        std::string errs;
        for (const auto& kv : v.obj) {
            std::string e;
            if (reg_.set(kv.first, kv.second, e)) ++applied;
            else errs += kv.first + ": " + e + "; ";
        }
        std::printf("Пресет '%s': применено %d параметров%s\n", name.c_str(), applied,
                    errs.empty() ? "" : ("; пропущено: " + errs).c_str());
        std::fflush(stdout);
        if (applied == 0) { err = errs.empty() ? "пустой пресет" : errs; return false; }
        return true;
    }

    bool save_preset(const std::string& name, std::string& err) {
        json::Value obj = json::Value::O();
        for (const auto& p : reg_.list().arr) {           // текущие значения всех ручек
            const json::Value* nm = p.find("name");
            const json::Value* val = p.find("value");
            if (nm && val) obj.set(nm->as_str(), *val);
        }
        const std::string path = presets_dir_ + "/" + name + ".json";
        std::ofstream f(path, std::ios::binary);
        if (!f) { err = "не могу записать " + path; return false; }
        f << json::dump(obj) << "\n";
        std::printf("Пресет '%s' сохранён в %s\n", name.c_str(), path.c_str());
        std::fflush(stdout);
        return true;
    }

    json::Value list_presets() {
        json::Value arr = json::Value::A();
        if (DIR* d = opendir(presets_dir_.c_str())) {
            while (struct dirent* e = readdir(d)) {
                std::string fn = e->d_name;
                if (fn.size() > 5 && fn.substr(fn.size() - 5) == ".json")
                    arr.arr.push_back(json::Value::S(fn.substr(0, fn.size() - 5)));
            }
            closedir(d);
        }
        return arr;
    }

    DahengCamera cam_;
    std::unique_ptr<H264Encoder> enc_;
    std::unique_ptr<RtspServer> rtsp_;
    std::unique_ptr<ControlServer> ctrl_;
    ParamRegistry reg_;

    int rtsp_port_, ctrl_port_;
    int maxside_;
    std::string presets_dir_ = "presets";
    std::string startup_preset_;

    std::mutex mu_;            // защищает конфиг ниже
    int src_w_ = 0, src_h_ = 0;
    int enc_w_ = 0, enc_h_ = 0, enc_fps_ = 30, enc_idr_ = 30, enc_bitrate_ = 6000000;
    bool enc_dirty_ = false;   // требуется пересоздание энкодера (коалесцирование width/height/fps)
    double exposure_us_ = 10000, gain_ = 0;
    bool auto_exp_ = true;
    int decim_ = 1;
    double exp_lo_ = 0, exp_hi_ = 0, gain_lo_ = 0, gain_hi_ = 0;

    std::mutex task_mu_;
    std::vector<std::function<void()>> tasks_;
    std::atomic<double> fps_meas_{0};

    std::mutex ts_mu_;                  // FIFO меток захвата (loop-поток пишет, DQ-поток читает)
    std::deque<int64_t> ts_fifo_;
};

int main(int argc, char** argv) {
    const std::string serial = (argc > 1) ? argv[1] : "";
    const int rtsp_port = (argc > 2) ? std::atoi(argv[2]) : 8554;
    const int ctrl_port = (argc > 3) ? std::atoi(argv[3]) : 8555;
    const int fps       = (argc > 4) ? std::atoi(argv[4]) : 30;
    const int maxside   = (argc > 5) ? std::atoi(argv[5]) : 1280;
    const std::string preset = (argc > 6) ? argv[6] : "";  // стартовый пресет (имя без .json)

    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);
    setvbuf(stdout, nullptr, _IOLBF, 0);  // построчный вывод — логи видны сразу по ssh

    CameraService svc(serial, rtsp_port, ctrl_port, fps, maxside, preset);
    if (!svc.init()) return 1;
    svc.run();
    std::printf("\nОстановка...\n");
    svc.shutdown();
    return 0;
}
