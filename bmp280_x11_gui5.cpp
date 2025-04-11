// bmp280_x11_gui5.cpp
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>
#include <ctime>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <cstring>
#include <cmath>
#include <sstream>
#include <memory>
#include <algorithm>
#include <array>
#include <optional>

#define WIDTH 800
#define HEIGHT 600
#define MAX_POINTS 300
#define BUFFER_SIZE 256
#define ERROR_DISPLAY_TIME 5
#define RECONNECT_TIMEOUT 5
#define STATS_WINDOW 300
#define HIGHLIGHT_DURATION 0.5

struct DataPoint {
    float temperature;
    float pressure;
    time_t timestamp;
};

struct Statistics {
    float min_temp, max_temp, avg_temp;
    float min_press, max_press, avg_press;
    int count;
};

class CircularBuffer {
    std::array<DataPoint, MAX_POINTS> buffer;
    size_t head = 0;
    size_t size = 0;
    mutable std::vector<float> temp_cache;
    mutable std::vector<float> press_cache;

public:
    CircularBuffer() : temp_cache(MAX_POINTS, 0.0f), press_cache(MAX_POINTS, 0.0f) {}
    void push(const DataPoint& point) {
        buffer[head] = point;
        head = (head + 1) % MAX_POINTS;
        if (size < MAX_POINTS) ++size;
        std::fill(temp_cache.begin(), temp_cache.end(), 0.0f);
        std::fill(press_cache.begin(), press_cache.end(), 0.0f);
    }
    size_t get_size() const { return size; }
    const DataPoint& operator[](size_t index) const {
        if (size == 0) throw std::out_of_range("Buffer is empty");
        size_t idx = (head + MAX_POINTS - size + index) % MAX_POINTS;
        return buffer[idx];
    }
    void clear() {
        head = 0;
        size = 0;
        std::fill(temp_cache.begin(), temp_cache.end(), 0.0f);
        std::fill(press_cache.begin(), press_cache.end(), 0.0f);
    }
    float smooth_value(bool is_temp, size_t index, int window = 5) const {
        if (size == 0) return 0.0f;
        index = std::min(index, size - 1);
        auto& cache = is_temp ? temp_cache : press_cache;
        if (cache[index] == 0.0f) {
            float sum = 0.0f;
            int count = 0;
            for (int i = std::max(0, static_cast<int>(index) - window / 2);
                 i <= std::min(static_cast<int>(size) - 1, static_cast<int>(index) + window / 2); ++i) {
                sum += is_temp ? buffer[(head + MAX_POINTS - size + i) % MAX_POINTS].temperature
                              : buffer[(head + MAX_POINTS - size + i) % MAX_POINTS].pressure;
                ++count;
            }
            cache[index] = count > 0 ? sum / count : (is_temp ? buffer[(head + MAX_POINTS - size + index) % MAX_POINTS].temperature
                                                             : buffer[(head + MAX_POINTS - size + index) % MAX_POINTS].pressure);
        }
        return cache[index];
    }
};

class SerialPort {
    int fd;
public:
    SerialPort(const std::string& port, speed_t baud) : fd(-1) {
        fd = open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd == -1) throw std::runtime_error("Failed to open serial port: " + port);

        termios tty{};
        if (tcgetattr(fd, &tty) != 0) {
            close(fd);
            throw std::runtime_error("Failed to get serial attributes");
        }
        cfsetispeed(&tty, baud);
        cfsetospeed(&tty, baud);
        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;
        tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_oflag &= ~OPOST;
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 1;

        if (tcsetattr(fd, TCSANOW, &tty) != 0) {
            close(fd);
            throw std::runtime_error("Failed to set serial attributes");
        }
    }
    ~SerialPort() { if (fd != -1) close(fd); }
    int get() const { return fd; }
    void close_port() { if (fd != -1) { close(fd); fd = -1; } }
};

class X11Display {
    Display* dpy = nullptr;
    Window win = 0;
    GC gc = 0;
    Pixmap pixmap = 0;

    void cleanup() {
        if (pixmap) { XFreePixmap(dpy, pixmap); pixmap = 0; }
        if (gc) { XFreeGC(dpy, gc); gc = 0; }
        if (win) { XDestroyWindow(dpy, win); win = 0; }
        if (dpy) { XCloseDisplay(dpy); dpy = nullptr; }
    }

public:
    X11Display() {
        dpy = XOpenDisplay(nullptr);
        if (!dpy) throw std::runtime_error("Cannot open display");

        int screen = DefaultScreen(dpy);
        win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 10, 10, WIDTH, HEIGHT, 1,
                                  BlackPixel(dpy, screen), WhitePixel(dpy, screen));
        XStoreName(dpy, win, "BMP280 Sensor Monitor");

        XWMHints* wm_hints = XAllocWMHints();
        if (wm_hints) {
            wm_hints->flags = StateHint;
            wm_hints->initial_state = NormalState;
            XSetWMHints(dpy, win, wm_hints);
            XFree(wm_hints);
        }

        XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
        XMapWindow(dpy, win);
        gc = XCreateGC(dpy, win, 0, nullptr);
        XSetForeground(dpy, gc, BlackPixel(dpy, screen));

        pixmap = XCreatePixmap(dpy, win, WIDTH, HEIGHT, DefaultDepth(dpy, screen));
        XSetForeground(dpy, gc, WhitePixel(dpy, screen));
        XFillRectangle(dpy, pixmap, gc, 0, 0, WIDTH, HEIGHT);
    }
    ~X11Display() { cleanup(); }
    X11Display(const X11Display&) = delete;
    X11Display& operator=(const X11Display&) = delete;
    Display* get_display() const { return dpy; }
    Window get_window() const { return win; }
    GC get_gc() const { return gc; }
    Pixmap get_pixmap() const { return pixmap; }
    void set_background(unsigned long color) {
        XSetWindowBackground(dpy, win, color);
        XSetForeground(dpy, gc, color);
        XFillRectangle(dpy, pixmap, gc, 0, 0, WIDTH, HEIGHT);
        XClearWindow(dpy, win);
    }
    void copy_pixmap_to_window() {
        XCopyArea(dpy, pixmap, win, gc, 0, 0, WIDTH, HEIGHT, 0, 0);
        XFlush(dpy);
    }
};

struct Config {
    speed_t baud_rate = B9600;
    int save_interval = 30;
    char csv_delimiter = ',';
    float temp_range[2] = {-40.0f, 85.0f};
    float press_range[2] = {300.0f, 1100.0f};
    std::string menu_bg_color = "#808080";
    std::string help_bg_color = "#D3D3D3";
    std::array<std::string, 4> graph_colors = {"blue", "red", "green", "yellow"};
};

struct GuiState {
    float zoom_temp, zoom_press, vzoom_temp, vzoom_press;
    int offset_temp, offset_press;
    int theme;
    bool show_help, paused;
    int selected_help_item;
    size_t history_size;

    bool operator!=(const GuiState& other) const {
        return zoom_temp != other.zoom_temp || zoom_press != other.zoom_press ||
               vzoom_temp != other.vzoom_temp || vzoom_press != other.vzoom_press ||
               offset_temp != other.offset_temp || offset_press != other.offset_press ||
               theme != other.theme || show_help != other.show_help ||
               paused != other.paused || selected_help_item != other.selected_help_item ||
               history_size != other.history_size;
    }
};

class BMP280Gui {
private:
    enum class Theme { White, Dark, HighContrast };

    std::unique_ptr<X11Display> x11;
    Display* dpy;
    Window win;
    GC gc;
    Pixmap pixmap;
    Window menu_win;
    GC menu_gc;
    std::unique_ptr<SerialPort> serial;
    int fd;
    CircularBuffer history;
    std::string filename;
    time_t last_save;
    std::array<unsigned long, 4> colors;
    unsigned long background_color;
    unsigned long text_color;
    unsigned long menu_bg_color;
    unsigned long menu_text_color;
    unsigned long menu_highlight_color;
    unsigned long help_bg_color;
    unsigned long keybind_color;
    Theme theme = Theme::White;
    float zoom_temp = 1.0f, zoom_press = 1.0f;
    float vzoom_temp = 1.0f, vzoom_press = 1.0f;
    int offset_temp = 0, offset_press = 0;
    mutable std::vector<std::string> error_messages;
    mutable std::vector<std::string> persistent_errors;
    mutable time_t last_error_time = 0;
    time_t last_reconnect_attempt = 0;
    int reconnect_attempts = 0;
    time_t menu_highlight_time = 0;
    float temp_range[2] = {-40.0f, 85.0f};
    float press_range[2] = {300.0f, 1100.0f};
    float default_temp_range[2] = {-40.0f, 85.0f};
    float default_press_range[2] = {300.0f, 1100.0f};
    speed_t baud_rate = B9600;
    int save_interval = 30;
    char csv_delimiter = ',';
    bool paused = false;
    bool window_mapped = false;
    bool show_help = false;
    int selected_help_item = -1;
    bool dragging = false;
    int drag_start_x = 0;
    bool needs_redraw = false;
    bool menu_needs_redraw = false;
    char serial_buffer[BUFFER_SIZE] = {0};
    size_t serial_buf_pos = 0;
    XFontStruct* regular_font = nullptr;
    XFontStruct* bold_font = nullptr;
    mutable unsigned long current_fg = 0;

    static constexpr std::array<std::string_view, 11> help_lines = {
        "Keyboard Shortcuts:",
        "q: Quit",
        "s: Save data to file",
        "p: Pause/Resume",
        "c: Clear errors",
        "b: Change baud rate",
        "+/-: Horizontal zoom in/out",
        "Up/Down: Vertical zoom in/out",
        "Left/Right: Scroll graph",
        "t: Toggle theme",
        "h: Show/hide this help"
    };
    static constexpr int max_reconnect_attempts = 10;

    static int x11_error_handler(Display* dpy, XErrorEvent* err) {
        char err_msg[256];
        XGetErrorText(dpy, err->error_code, err_msg, sizeof(err_msg));
        std::ofstream log("logs/errors.log", std::ios::app);
        if (log) {
            time_t now = time(nullptr);
            log << std::ctime(&now) << ": X11 error: " << err_msg << "\n";
        }
        std::cerr << "X11 error: " << err_msg << "\n";
        if (err->error_code == BadWindow || err->error_code == BadDrawable) {
            throw std::runtime_error("Critical X11 error: " + std::string(err_msg));
        }
        return 0;
    }

    void add_error(const std::string& msg, bool persistent = false) const {
        if (error_messages.size() >= 5) error_messages.erase(error_messages.begin());
        error_messages.push_back(msg);
        if (persistent) {
            if (persistent_errors.size() >= 5) persistent_errors.erase(persistent_errors.begin());
            persistent_errors.push_back(msg);
        }
        last_error_time = time(nullptr);
        std::ofstream log("logs/errors.log", std::ios::app);
        if (log) log << std::ctime(&last_error_time) << ": " << msg << "\n";
    }

    std::optional<std::string> find_serial_port() const {
        static const char* prefixes[] = {"/dev/ttyACM", "/dev/ttyUSB"};
        for (const auto* prefix : prefixes) {
            for (int i = 0; i < 10; ++i) {
                std::string port = std::string(prefix) + std::to_string(i);
                if (std::filesystem::exists(port)) return port;
            }
        }
        return std::nullopt;
    }

    bool open_serial(const std::string& port, speed_t baud) {
        try {
            serial = std::make_unique<SerialPort>(port, baud);
            fd = serial->get();
            return true;
        } catch (const std::exception& e) {
            add_error(e.what(), true);
            fd = -1;
            return false;
        }
    }

    void try_reconnect() {
        if (fd != -1 || reconnect_attempts >= max_reconnect_attempts) return;
        if (difftime(time(nullptr), last_reconnect_attempt) < RECONNECT_TIMEOUT) return;
        last_reconnect_attempt = time(nullptr);
        reconnect_attempts++;

        auto port = find_serial_port();
        if (!port) {
            add_error("No serial port available", true);
            return;
        }

        speed_t baud_rates[] = {B9600, B115200};
        for (speed_t baud : baud_rates) {
            if (open_serial(*port, baud)) {
                baud_rate = baud;
                std::cout << "Reconnected to " << *port << " at baud rate " << (baud == B9600 ? 9600 : 115200) << "\n";
                persistent_errors.clear();
                error_messages.clear();
                reconnect_attempts = 0;
                return;
            }
        }
        add_error("Failed to reconnect to " + *port + " with any baud rate", true);
    }

    bool parse_value(const std::string& line, float& value) const {
        size_t pos = line.find_first_of("-0123456789");
        if (pos == std::string::npos) return false;
        try {
            value = std::stof(line.substr(pos));
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    void process_line(const std::string& line, float& temp, float& press, bool& got_temp, bool& got_press) {
        float value;
        if (line.find("Temp") != std::string::npos && parse_value(line, value)) {
            if (value >= -40.0f && value <= 85.0f) {
                temp = value;
                got_temp = true;
            } else {
                add_error("Invalid temperature: " + std::to_string(value));
            }
        } else if (line.find("Pres") != std::string::npos && parse_value(line, value)) {
            if (value >= 300.0f && value <= 1100.0f) {
                press = value;
                got_press = true;
            } else {
                add_error("Invalid pressure: " + std::to_string(value));
            }
        }
    }

    void read_serial() {
        if (fd == -1 || paused) return;

        fd_set set;
        struct timeval timeout = {0, 100000};
        FD_ZERO(&set);
        FD_SET(fd, &set);

        int ready = select(fd + 1, &set, nullptr, nullptr, &timeout);
        if (ready < 0) {
            add_error("Select error: " + std::string(strerror(errno)));
            serial->close_port();
            fd = -1;
            return;
        }
        if (ready == 0) return;

        int len = read(fd, serial_buffer + serial_buf_pos, BUFFER_SIZE - serial_buf_pos - 1);
        if (len < 0 && errno != EAGAIN) {
            add_error("Serial read error: " + std::string(strerror(errno)));
            serial->close_port();
            fd = -1;
            return;
        }
        if (len <= 0) return;

        serial_buffer[serial_buf_pos + len] = '\0';
        std::string buf(serial_buffer, serial_buf_pos + len);
        size_t pos = 0;
        float temp = 0.0f, press = 0.0f;
        bool got_temp = false, got_press = false;

        while (pos < buf.size()) {
            size_t nl = buf.find('\n', pos);
            if (nl == std::string::npos) break;
            std::string line = buf.substr(pos, nl - pos);
            pos = nl + 1;
            process_line(line, temp, press, got_temp, got_press);
        }

        if (got_temp && got_press) {
            history.push({temp, press, time(nullptr)});
            log_data();
        }

        serial_buf_pos = buf.size() - pos;
        if (serial_buf_pos > 0) {
            std::memmove(serial_buffer, buf.c_str() + pos, serial_buf_pos);
            serial_buffer[serial_buf_pos] = '\0';
        } else {
            serial_buf_pos = 0;
        }
    }

    void log_data() const {
        if (history.get_size() == 0) return;
        const auto& last = history[history.get_size() - 1];
        float altitude = 44330.0f * (1.0f - std::pow(last.pressure / 1013.25f, 0.1903f));
        std::cout << "Temp: " << last.temperature << " C, Press: " << last.pressure
                  << " hPa, Alt: " << altitude << " m\n";
    }

    float compute_visible_average(bool is_temp, int start, int max_points) const {
        if (history.get_size() == 0) return 0.0f;
        float sum = 0.0f;
        int count = 0;
        for (int i = start; i < start + max_points && static_cast<size_t>(i) < history.get_size(); ++i) {
            sum += is_temp ? history[i].temperature : history[i].pressure;
            ++count;
        }
        return count > 0 ? sum / count : 0.0f;
    }

    void set_foreground(unsigned long color) const {
        if (color != current_fg) {
            XSetForeground(dpy, gc, color);
            current_fg = color;
        }
    }

    void draw_graph(int x, int y, int w, int h, bool is_temp, float threshold, unsigned long color_low, unsigned long color_high) const {
        if (history.get_size() < 2) return;

        float zoom = std::clamp(is_temp ? zoom_temp : zoom_press, 1.0f, 100.0f);
        int offset = std::clamp(is_temp ? offset_temp : offset_press, 0, static_cast<int>(history.get_size()));
        int max_points = static_cast<int>(MAX_POINTS / zoom);
        int start = std::max(0, std::min(static_cast<int>(history.get_size()) - 2, static_cast<int>(history.get_size()) - max_points - offset));

        float vzoom = std::clamp(is_temp ? vzoom_temp : vzoom_press, 1.0f, 100.0f);
        const float* default_range = is_temp ? default_temp_range : default_press_range;
        float default_min = default_range[0];
        float default_max = default_range[1];
        float default_span = default_max - default_min;

        float avg_val = compute_visible_average(is_temp, start, max_points);
        float span = default_span / vzoom;
        float min_val = avg_val - span / 2.0f;
        float max_val = avg_val + span / 2.0f;

        if (min_val < default_min) {
            min_val = default_min;
            max_val = min_val + span;
        }
        if (max_val > default_max) {
            max_val = default_max;
            min_val = max_val - span;
        }

        set_foreground(theme == Theme::White ? 0xCCCCCC : 0x555555);
        for (int i = 1; i < 5; ++i) {
            int y_pos = y + i * h / 5;
            XDrawLine(dpy, pixmap, gc, x, y_pos, x + w, y_pos);
            int x_pos = x + i * w / 5;
            XDrawLine(dpy, pixmap, gc, x_pos, y, x_pos, y + h);
        }

        set_foreground(text_color);
        XDrawRectangle(dpy, pixmap, gc, x, y, w, h);

        for (int i = 1; i < max_points && static_cast<size_t>(start + i) < history.get_size(); ++i) {
            float val0 = history.smooth_value(is_temp, start + i - 1);
            float val1 = history.smooth_value(is_temp, start + i);
            int x0 = x + (i - 1) * w / max_points;
            int x1 = x + i * w / max_points;
            int y0 = y + h - static_cast<int>((val0 - min_val) / (max_val - min_val) * h);
            int y1 = y + h - static_cast<int>((val1 - min_val) / (max_val - min_val) * h);
            y0 = std::max(y, std::min(y + h, y0));
            y1 = std::max(y, std::min(y + h, y1));
            set_foreground(is_temp ? (val1 > threshold ? color_high : color_low)
                                  : (std::abs(val1 - val0) > 1.0f ? color_high : color_low));
            XDrawLine(dpy, pixmap, gc, x0, y0, x1, y1);
        }

        set_foreground(text_color);
        for (int i = 0; i <= 5; ++i) {
            float val = min_val + i * (max_val - min_val) / 5;
            int y_pos = y + h - i * h / 5;
            char label[32];
            snprintf(label, sizeof(label), "%.0f %s", val, is_temp ? "C" : "hPa");
            XDrawLine(dpy, pixmap, gc, x - 5, y_pos, x, y_pos);
            XDrawString(dpy, pixmap, gc, x - 50, y_pos + 4, label, strlen(label));
        }

        if (history.get_size() >= 2) {
            time_t start_time = history[start].timestamp;
            time_t end_time = history[std::min(static_cast<size_t>(start + max_points - 1), history.get_size() - 1)].timestamp;
            for (int i = 0; i <= 5; ++i) {
                int x_pos = x + i * w / 5;
                time_t t = start_time + (end_time - start_time) * i / 5;
                char time_str[16];
                strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&t));
                XDrawString(dpy, pixmap, gc, x_pos - 20, y + h + 15, time_str, strlen(time_str));
            }
        }

        set_foreground(text_color);
        const char* label = is_temp ? "Temperature" : "Pressure";
        XDrawString(dpy, pixmap, gc, x + 10, y + 15, label, strlen(label));
        set_foreground(color_low);
        XDrawLine(dpy, pixmap, gc, x + 100, y + 10, x + 120, y + 10);
        if (is_temp) {
            set_foreground(color_high);
            XDrawLine(dpy, pixmap, gc, x + 130, y + 10, x + 150, y + 10);
        }
    }

    void draw_menu_bar() const {
        XWindowAttributes attrs;
        XGetWindowAttributes(dpy, menu_win, &attrs);

        bool is_highlighted = difftime(time(nullptr), menu_highlight_time) <= HIGHLIGHT_DURATION;
        XSetForeground(dpy, menu_gc, is_highlighted ? menu_highlight_color : menu_bg_color);
        XFillRectangle(dpy, menu_win, menu_gc, 0, 0, attrs.width, attrs.height);

        std::stringstream ss;
        ss << "File: " << filename << " | Interval: " << save_interval
           << "s | Port: " << (fd != -1 ? "Connected" : "Disconnected")
           << " | HZoom: " << std::fixed << std::setprecision(2) << zoom_temp
           << " | VZoom: " << vzoom_temp
           << " | Offset: " << offset_temp
           << (paused ? " | Paused" : "")
           << " | Theme: " << (theme == Theme::White ? "White" : theme == Theme::Dark ? "Dark" : "High-Contrast")
           << " | Press 'h' for help";
        std::string status = ss.str();
        XSetForeground(dpy, menu_gc, menu_text_color);
        XDrawString(dpy, menu_win, menu_gc, 10, 20, status.c_str(), status.length());
    }

    void save_data() {
        std::filesystem::create_directory("logs");
        std::string temp_path = "logs/" + filename + ".tmp";
        std::ofstream out(temp_path);
        if (!out) {
            add_error("Failed to open temp file: " + temp_path);
            return;
        }
        for (size_t i = 0; i < history.get_size(); ++i) {
            const auto& d = history[i];
            out << d.temperature << csv_delimiter << d.pressure << csv_delimiter << d.timestamp << "\n";
        }
        out.close();
        if (out.fail()) {
            add_error("Failed to write data to temp file");
            return;
        }
        try {
            std::filesystem::rename(temp_path, "logs/" + filename);
            add_error("Saved to logs/" + filename);
        } catch (const std::exception& e) {
            add_error("Failed to rename temp file: " + std::string(e.what()));
        }
    }

    bool load_data(const std::string& path) {
        if (!std::filesystem::exists(path)) return false;

        history.clear();
        std::ifstream in(path);
        if (!in) {
            add_error("Failed to open data file: " + path);
            return false;
        }
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream iss(line);
            float t, p;
            time_t ts;
            char delim;
            if (iss >> t >> delim >> p >> delim >> ts && delim == csv_delimiter &&
                t >= -40.0f && t <= 85.0f && p >= 300.0f && p <= 1100.0f &&
                ts > 0 && ts <= time(nullptr)) {
                history.push({t, p, ts});
            } else {
                add_error("Invalid data line: " + line);
            }
        }
        return history.get_size() > 0;
    }

    Statistics calculate_statistics() const {
        Statistics stats = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0};
        if (history.get_size() == 0) return stats;

        time_t now = time(nullptr);
        float temp_sum = 0.0f, press_sum = 0.0f;
        bool first = true;

        for (size_t i = 0; i < history.get_size(); ++i) {
            const auto& point = history[i];
            if (difftime(now, point.timestamp) > STATS_WINDOW) continue;

            if (first) {
                stats.min_temp = stats.max_temp = point.temperature;
                stats.min_press = stats.max_press = point.pressure;
                first = false;
            } else {
                stats.min_temp = std::min(stats.min_temp, point.temperature);
                stats.max_temp = std::max(stats.max_temp, point.temperature);
                stats.min_press = std::min(stats.min_press, point.pressure);
                stats.max_press = std::max(stats.max_press, point.pressure);
            }
            temp_sum += point.temperature;
            press_sum += point.pressure;
            stats.count++;
        }

        if (stats.count > 0) {
            stats.avg_temp = temp_sum / stats.count;
            stats.avg_press = press_sum / stats.count;
        }
        return stats;
    }

    void draw_footer() const {
        if (history.get_size() == 0) return;
        const auto& last = history[history.get_size() - 1];
        auto stats = calculate_statistics();
        char info[256];
        float altitude = 44330.0f * (1.0f - std::pow(last.pressure / 1013.25f, 0.1903f));
        snprintf(info, sizeof(info),
                 "Last: T=%.1f C, P=%.1f hPa, A=%.1f m | 5min: T(min/max/avg)=%.1f/%.1f/%.1f C, P(min/max/avg)=%.1f/%.1f/%.1f hPa",
                 last.temperature, last.pressure, altitude,
                 stats.min_temp, stats.max_temp, stats.avg_temp,
                 stats.min_press, stats.max_press, stats.avg_press);
        set_foreground(text_color);
        XDrawString(dpy, pixmap, gc, 20, HEIGHT - 20, info, strlen(info));
    }

    void draw_errors() const {
        if (error_messages.empty() && persistent_errors.empty()) return;
        set_foreground(colors[0]);
        int y = 40;
        for (const auto& msg : persistent_errors) {
            XDrawString(dpy, pixmap, gc, 10, y, msg.c_str(), msg.length());
            y += 15;
        }
        if (difftime(time(nullptr), last_error_time) <= ERROR_DISPLAY_TIME) {
            for (const auto& msg : error_messages) {
                XDrawString(dpy, pixmap, gc, 10, y, msg.c_str(), msg.length());
                y += 15;
            }
        }
    }

    void draw_help() const {
        if (!show_help) return;

        const int line_height = 15;
        const int padding = 10;

        int max_width = 0;
        for (const auto& line : help_lines) {
            int width = XTextWidth(regular_font, line.data(), line.length());
            if (width > max_width) max_width = width;
        }
        int total_height = help_lines.size() * line_height;
        int rect_width = max_width + 2 * padding;
        int rect_height = total_height + 2 * padding;

        int start_x = WIDTH / 2 - rect_width / 2;
        int start_y = HEIGHT / 2 - rect_height / 2;

        set_foreground(help_bg_color);
        XFillRectangle(dpy, pixmap, gc, start_x, start_y, rect_width, rect_height);

        set_foreground(text_color);
        XDrawRectangle(dpy, pixmap, gc, start_x, start_y, rect_width - 1, rect_height - 1);

        int y = start_y + padding + line_height - 5;
        for (size_t i = 0; i < help_lines.size(); ++i) {
            const auto& line = help_lines[i];
            int text_width = XTextWidth(i == 0 ? bold_font : regular_font, line.data(), line.length());
            int text_x = start_x + (rect_width - text_width) / 2;

            if (static_cast<int>(i) == selected_help_item) {
                set_foreground(menu_highlight_color);
                XFillRectangle(dpy, pixmap, gc, start_x + padding, y - line_height + 5, rect_width - 2 * padding, line_height);
            }

            if (i == 0) {
                XSetFont(dpy, gc, bold_font->fid);
                set_foreground(text_color);
                XDrawString(dpy, pixmap, gc, text_x, y, line.data(), line.length());
            } else {
                XSetFont(dpy, gc, regular_font->fid);
                std::string str(line);
                size_t colon_pos = str.find(": ");
                if (colon_pos != std::string::npos) {
                    std::string keybind = str.substr(0, colon_pos);
                    std::string desc = str.substr(colon_pos + 2);
                    set_foreground(keybind_color);
                    XDrawString(dpy, pixmap, gc, text_x, y, keybind.c_str(), keybind.length());
                    set_foreground(text_color);
                    XDrawString(dpy, pixmap, gc, text_x + XTextWidth(regular_font, keybind.c_str(), keybind.length()) + 5, y, desc.c_str(), desc.length());
                } else {
                    set_foreground(text_color);
                    XDrawString(dpy, pixmap, gc, text_x, y, line.data(), line.length());
                }
            }
            y += line_height;
        }
    }

    void load_fonts() {
        regular_font = XLoadQueryFont(dpy, "fixed");
        if (!regular_font) {
            regular_font = XLoadQueryFont(dpy, "6x13");
            if (!regular_font) add_error("Failed to load regular font");
        }
        bold_font = XLoadQueryFont(dpy, "-*-helvetica-bold-r-*-*-12-*-*-*-*-*-*-*");
        if (!bold_font) {
            bold_font = regular_font;
            add_error("Failed to load bold font, using regular font");
        }
    }

    void free_fonts() {
        if (regular_font && regular_font != bold_font) XFreeFont(dpy, regular_font);
        if (bold_font) XFreeFont(dpy, bold_font);
    }

    void create_default_config(const std::string& path) const {
        std::ofstream out(path);
        if (!out) {
            add_error("Failed to create default config file: " + path);
            return;
        }
        out << "baud_rate=9600\n"
            << "save_interval=30\n"
            << "temp_min=-40\n"
            << "temp_max=85\n"
            << "press_min=300\n"
            << "press_max=1100\n"
            << "csv_delimiter=,\n"
            << "menu_bg_color=#808080\n"
            << "help_bg_color=#D3D3D3\n"
            << "graph_color_temp_low=blue\n"
            << "graph_color_temp_high=red\n"
            << "graph_color_press_low=green\n"
            << "graph_color_press_high=yellow\n";
        out.close();
        if (out.fail()) {
            add_error("Failed to write default config file: " + path);
        }
    }

    bool load_config(const std::string& path) {
        if (!std::filesystem::exists(path)) {
            create_default_config(path);
        }

        std::ifstream in(path);
        if (!in) {
            add_error("Failed to open config: " + path);
            return false;
        }
        Config config;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            try {
                if (line.find("baud_rate=") == 0) {
                    int baud = std::stoi(line.substr(10));
                    if (baud == 9600) config.baud_rate = B9600;
                    else if (baud == 115200) config.baud_rate = B115200;
                    else {
                        config.baud_rate = B9600;
                        add_error("Invalid baud rate: " + std::to_string(baud));
                    }
                } else if (line.find("save_interval=") == 0) {
                    config.save_interval = std::stoi(line.substr(14));
                    if (config.save_interval < 1 || config.save_interval > 3600) {
                        config.save_interval = 30;
                        add_error("Invalid save interval: " + std::to_string(config.save_interval));
                    }
                } else if (line.find("temp_min=") == 0) {
                    config.temp_range[0] = std::stof(line.substr(9));
                    if (config.temp_range[0] < -40.0f || config.temp_range[0] > 85.0f) {
                        config.temp_range[0] = -40.0f;
                        add_error("Invalid temp_min: " + line.substr(9));
                    }
                } else if (line.find("temp_max=") == 0) {
                    config.temp_range[1] = std::stof(line.substr(9));
                    if (config.temp_range[1] <= config.temp_range[0] || config.temp_range[1] > 85.0f) {
                        config.temp_range[1] = 85.0f;
                        add_error("Invalid temp_max: " + line.substr(9));
                    }
                } else if (line.find("press_min=") == 0) {
                    config.press_range[0] = std::stof(line.substr(10));
                    if (config.press_range[0] < 300.0f || config.press_range[0] > 1100.0f) {
                        config.press_range[0] = 300.0f;
                        add_error("Invalid press_min: " + line.substr(10));
                    }
                } else if (line.find("press_max=") == 0) {
                    config.press_range[1] = std::stof(line.substr(10));
                    if (config.press_range[1] <= config.press_range[0] || config.press_range[1] > 1100.0f) {
                        config.press_range[1] = 1100.0f;
                        add_error("Invalid press_max: " + line.substr(10));
                    }
                } else if (line.find("csv_delimiter=") == 0 && !line.substr(14).empty()) {
                    config.csv_delimiter = line.substr(14)[0];
                } else if (line.find("menu_bg_color=") == 0) {
                    config.menu_bg_color = line.substr(14);
                } else if (line.find("help_bg_color=") == 0) {
                    config.help_bg_color = line.substr(14);
                } else if (line.find("graph_color_temp_low=") == 0) {
                    config.graph_colors[0] = line.substr(20);
                } else if (line.find("graph_color_temp_high=") == 0) {
                    config.graph_colors[1] = line.substr(21);
                } else if (line.find("graph_color_press_low=") == 0) {
                    config.graph_colors[2] = line.substr(21);
                } else if (line.find("graph_color_press_high=") == 0) {
                    config.graph_colors[3] = line.substr(22);
                }
            } catch (const std::exception& e) {
                add_error("Invalid config line: " + line);
            }
        }

        baud_rate = config.baud_rate;
        save_interval = config.save_interval;
        csv_delimiter = config.csv_delimiter;
        std::copy(config.temp_range, config.temp_range + 2, temp_range);
        std::copy(config.temp_range, config.temp_range + 2, default_temp_range);
        std::copy(config.press_range, config.press_range + 2, press_range);
        std::copy(config.press_range, config.press_range + 2, default_press_range);
        update_theme(config.menu_bg_color, config.help_bg_color, config.graph_colors);
        return true;
    }

    void update_theme(const std::string& custom_menu_bg_color,
                     const std::string& custom_help_bg_color,
                     const std::array<std::string, 4>& custom_colors) {
        Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
        XColor color;

        if (theme == Theme::White) {
            background_color = WhitePixel(dpy, DefaultScreen(dpy));
            text_color = BlackPixel(dpy, DefaultScreen(dpy));
            menu_text_color = WhitePixel(dpy, DefaultScreen(dpy));
            const char* default_colors[] = {
                custom_colors[0].empty() ? "blue" : custom_colors[0].c_str(),
                custom_colors[1].empty() ? "red" : custom_colors[1].c_str(),
                custom_colors[2].empty() ? "green" : custom_colors[2].c_str(),
                custom_colors[3].empty() ? "yellow" : custom_colors[3].c_str()
            };
            for (int i = 0; i < 4; ++i) {
                if (XParseColor(dpy, cmap, default_colors[i], &color) && XAllocColor(dpy, cmap, &color)) {
                    colors[i] = color.pixel;
                } else {
                    colors[i] = BlackPixel(dpy, DefaultScreen(dpy));
                    add_error("Failed to allocate graph color: " + std::string(default_colors[i]));
                }
            }
            const char* help_color = custom_help_bg_color.empty() ? "#D3D3D3" : custom_help_bg_color.c_str();
            if (XParseColor(dpy, cmap, help_color, &color) && XAllocColor(dpy, cmap, &color)) {
                help_bg_color = color.pixel;
            } else {
                help_bg_color = WhitePixel(dpy, DefaultScreen(dpy));
                add_error("Failed to allocate help background color: " + std::string(help_color));
            }
            if (XParseColor(dpy, cmap, "darkblue", &color) && XAllocColor(dpy, cmap, &color)) {
                keybind_color = color.pixel;
            } else {
                keybind_color = text_color;
            }
        } else if (theme == Theme::Dark) {
            if (XParseColor(dpy, cmap, "#333333", &color) && XAllocColor(dpy, cmap, &color)) {
                background_color = color.pixel;
            } else {
                background_color = BlackPixel(dpy, DefaultScreen(dpy));
            }
            if (XParseColor(dpy, cmap, "#BBBBBB", &color) && XAllocColor(dpy, cmap, &color)) {
                text_color = color.pixel;
            } else {
                text_color = WhitePixel(dpy, DefaultScreen(dpy));
            }
            menu_text_color = BlackPixel(dpy, DefaultScreen(dpy));
            const char* default_colors[] = {
                custom_colors[0].empty() ? "#55AAFF" : custom_colors[0].c_str(),
                custom_colors[1].empty() ? "#FF5555" : custom_colors[1].c_str(),
                custom_colors[2].empty() ? "#55FF55" : custom_colors[2].c_str(),
                custom_colors[3].empty() ? "#FFFF55" : custom_colors[3].c_str()
            };
            for (int i = 0; i < 4; ++i) {
                if (XParseColor(dpy, cmap, default_colors[i], &color) && XAllocColor(dpy, cmap, &color)) {
                    colors[i] = color.pixel;
                } else {
                    colors[i] = WhitePixel(dpy, DefaultScreen(dpy));
                    add_error("Failed to allocate graph color: " + std::string(default_colors[i]));
                }
            }
            const char* help_color = custom_help_bg_color.empty() ? "#555555" : custom_help_bg_color.c_str();
            if (XParseColor(dpy, cmap, help_color, &color) && XAllocColor(dpy, cmap, &color)) {
                help_bg_color = color.pixel;
            } else {
                help_bg_color = BlackPixel(dpy, DefaultScreen(dpy));
                add_error("Failed to allocate help background color: " + std::string(help_color));
            }
            if (XParseColor(dpy, cmap, "lightblue", &color) && XAllocColor(dpy, cmap, &color)) {
                keybind_color = color.pixel;
            } else {
                keybind_color = text_color;
            }
        } else {
            background_color = BlackPixel(dpy, DefaultScreen(dpy));
            text_color = WhitePixel(dpy, DefaultScreen(dpy));
            menu_text_color = WhitePixel(dpy, DefaultScreen(dpy));
            const char* default_colors[] = {
                custom_colors[0].empty() ? "cyan" : custom_colors[0].c_str(),
                custom_colors[1].empty() ? "magenta" : custom_colors[1].c_str(),
                custom_colors[2].empty() ? "lime" : custom_colors[2].c_str(),
                custom_colors[3].empty() ? "yellow" : custom_colors[3].c_str()
            };
            for (int i = 0; i < 4; ++i) {
                if (XParseColor(dpy, cmap, default_colors[i], &color) && XAllocColor(dpy, cmap, &color)) {
                    colors[i] = color.pixel;
                } else {
                    colors[i] = WhitePixel(dpy, DefaultScreen(dpy));
                    add_error("Failed to allocate graph color: " + std::string(default_colors[i]));
                }
            }
            const char* help_color = custom_help_bg_color.empty() ? "#333333" : custom_help_bg_color.c_str();
            if (XParseColor(dpy, cmap, help_color, &color) && XAllocColor(dpy, cmap, &color)) {
                help_bg_color = color.pixel;
            } else {
                help_bg_color = BlackPixel(dpy, DefaultScreen(dpy));
                add_error("Failed to allocate help background color: " + std::string(help_color));
            }
            if (XParseColor(dpy, cmap, "yellow", &color) && XAllocColor(dpy, cmap, &color)) {
                keybind_color = color.pixel;
            } else {
                keybind_color = text_color;
            }
        }

        const char* menu_color = custom_menu_bg_color.empty() ? "#808080" : custom_menu_bg_color.c_str();
        if (XParseColor(dpy, cmap, menu_color, &color) && XAllocColor(dpy, cmap, &color)) {
            menu_bg_color = color.pixel;
        } else {
            if (XParseColor(dpy, cmap, "#A9A9A9", &color) && XAllocColor(dpy, cmap, &color)) {
                menu_bg_color = color.pixel;
            } else {
                menu_bg_color = background_color;
                add_error("Failed to allocate menu background color: " + std::string(menu_color));
            }
        }

        if (XParseColor(dpy, cmap, theme == Theme::White ? "#A0A0A0" : "#606060", &color) && XAllocColor(dpy, cmap, &color)) {
            menu_highlight_color = color.pixel;
        } else {
            menu_highlight_color = menu_bg_color;
        }

        x11->set_background(background_color);
        XSetWindowBackground(dpy, menu_win, menu_bg_color);
        XClearWindow(dpy, menu_win);
    }

    void handle_events() {
        while (XPending(dpy)) {
            XEvent evt;
            XNextEvent(dpy, &evt);
            if (evt.type == Expose) {
                if (evt.xexpose.window == win) {
                    needs_redraw = true;
                } else if (evt.xexpose.window == menu_win) {
                    menu_needs_redraw = true;
                }
            }
            if (evt.type == KeyPress) {
                char keybuf[8];
                KeySym key;
                XLookupString(&evt.xkey, keybuf, sizeof(keybuf), &key, nullptr);
                menu_highlight_time = time(nullptr);
                menu_needs_redraw = true;
                if (key == XK_q || key == XK_Q) throw std::runtime_error("User quit");
                if (key == XK_s || key == XK_S) {
                    std::cout << "Enter filename to save (empty to keep " << filename << "): ";
                    std::string input;
                    std::getline(std::cin, input);
                    if (!input.empty()) filename = input;
                    save_data();
                    needs_redraw = true;
                }
                if (key == XK_p || key == XK_P) {
                    paused = !paused;
                    needs_redraw = true;
                }
                if (key == XK_c || key == XK_C) {
                    error_messages.clear();
                    if (fd != -1) persistent_errors.clear();
                    needs_redraw = true;
                }
                if (key == XK_b || key == XK_B) {
                    std::cout << "Enter baud rate (9600 or 115200): ";
                    std::string input;
                    std::getline(std::cin, input);
                    try {
                        int baud = std::stoi(input);
                        speed_t new_baud = baud == 115200 ? B115200 : B9600;
                        if (baud != 9600 && baud != 115200) {
                            add_error("Invalid baud rate, using default: 9600");
                        } else {
                            baud_rate = new_baud;
                            if (fd != -1) {
                                serial->close_port();
                                fd = -1;
                                try_reconnect();
                            }
                            add_error("Set baud rate to: " + std::to_string(baud));
                        }
                    } catch (const std::exception&) {
                        add_error("Invalid baud rate input");
                    }
                    needs_redraw = true;
                }
                if (key == XK_h || key == XK_H) {
                    show_help = !show_help;
                    selected_help_item = show_help ? 1 : -1;
                    needs_redraw = true;
                }
                if (key == XK_t || key == XK_T) {
                    theme = static_cast<Theme>((static_cast<int>(theme) + 1) % 3);
                    update_theme("", "", {});
                    needs_redraw = true;
                    menu_needs_redraw = true;
                }
                if (key == XK_plus || key == XK_KP_Add) {
                    zoom_temp = std::min(10.0f, zoom_temp * 1.5f);
                    zoom_press = std::min(10.0f, zoom_press * 1.5f);
                    offset_temp = 0;
                    offset_press = 0;
                    needs_redraw = true;
                } else if (key == XK_minus || key == XK_KP_Subtract) {
                    zoom_temp = std::max(1.0f, zoom_temp / 1.5f);
                    zoom_press = std::max(1.0f, zoom_press / 1.5f);
                    offset_temp = 0;
                    offset_press = 0;
                    needs_redraw = true;
                } else if (key == XK_Up) {
                    if (show_help) {
                        selected_help_item = std::max(1, selected_help_item - 1);
                    } else {
                        vzoom_temp = std::min(10.0f, vzoom_temp * 1.5f);
                        vzoom_press = std::min(10.0f, vzoom_press * 1.5f);
                    }
                    needs_redraw = true;
                } else if (key == XK_Down) {
                    if (show_help) {
                        selected_help_item = std::min(static_cast<int>(help_lines.size()) - 1, selected_help_item + 1);
                    } else {
                        vzoom_temp = std::max(1.0f, vzoom_temp / 1.5f);
                        vzoom_press = std::max(1.0f, vzoom_press / 1.5f);
                    }
                    needs_redraw = true;
                } else if (key == XK_Left) {
                    int max_offset = static_cast<int>(history.get_size()) - static_cast<int>(MAX_POINTS / zoom_temp);
                    offset_temp = std::min(offset_temp + 10, std::max(0, max_offset));
                    offset_press = std::min(offset_press + 10, std::max(0, max_offset));
                    needs_redraw = true;
                } else if (key == XK_Right) {
                    offset_temp = std::max(0, offset_temp - 10);
                    offset_press = std::max(0, offset_press - 10);
                    needs_redraw = true;
                }
            }
            if (evt.type == ButtonPress) {
                if (evt.xbutton.window == menu_win) {
                    menu_highlight_time = time(nullptr);
                    menu_needs_redraw = true;
                    int x = evt.xbutton.x;
                    if (x < 100) {
                        std::cout << "Enter filename to save (empty to keep " << filename << "): ";
                        std::string input;
                        std::getline(std::cin, input);
                        if (!input.empty()) filename = input;
                        save_data();
                    } else if (x < 200) {
                        paused = !paused;
                        needs_redraw = true;
                    }
                } else if (evt.xbutton.window == win) {
                    int x = evt.xbutton.x, y = evt.xbutton.y;
                    if (show_help) {
                        const int line_height = 15;
                        const int padding = 10;
                        int max_width = 0;
                        for (const auto& line : help_lines) {
                            int width = XTextWidth(regular_font, line.data(), line.length());
                            if (width > max_width) max_width = width;
                        }
                        int rect_width = max_width + 2 * padding;
                        int rect_height = help_lines.size() * line_height + 2 * padding;
                        int start_x = WIDTH / 2 - rect_width / 2;
                        int start_y = HEIGHT / 2 - rect_height / 2;
                        if (x >= start_x && x <= start_x + rect_width &&
                            y >= start_y && y <= start_y + rect_height) {
                            int item = (y - start_y - padding) / line_height + 1;
                            if (item >= 1 && item < static_cast<int>(help_lines.size())) {
                                selected_help_item = item;
                                needs_redraw = true;
                            }
                        }
                    }
                    bool on_temp_graph = (x >= 100 && x <= 700 && y >= 40 && y <= 240);
                    bool on_press_graph = (x >= 100 && x <= 700 && y >= 290 && y <= 490);
                    if (evt.xbutton.button == Button1 && (on_temp_graph || on_press_graph)) {
                        if (on_temp_graph) {
                            zoom_temp = std::min(10.0f, zoom_temp * 1.5f);
                            offset_temp = 0;
                        }
                        if (on_press_graph) {
                            zoom_press = std::min(10.0f, zoom_press * 1.5f);
                            offset_press = 0;
                        }
                        needs_redraw = true;
                    } else if (evt.xbutton.button == Button3 && (on_temp_graph || on_press_graph)) {
                        if (on_temp_graph) {
                            zoom_temp = std::max(1.0f, zoom_temp / 1.5f);
                            offset_temp = 0;
                        }
                        if (on_press_graph) {
                            zoom_press = std::max(1.0f, zoom_press / 1.5f);
                            offset_press = 0;
                        }
                        needs_redraw = true;
                    } else if (evt.xbutton.button == Button2 && (on_temp_graph || on_press_graph)) {
                        dragging = true;
                        drag_start_x = x;
                    }
                }
            }
            if (evt.type == ButtonRelease && evt.xbutton.button == Button2) {
                dragging = false;
            }
            if (evt.type == MotionNotify && dragging) {
                int x = evt.xmotion.x;
                int delta = (drag_start_x - x) / 10;
                int max_offset = static_cast<int>(history.get_size()) - static_cast<int>(MAX_POINTS / zoom_temp);
                offset_temp = std::clamp(offset_temp + delta, 0, max_offset);
                offset_press = std::clamp(offset_press + delta, 0, max_offset);
                drag_start_x = x;
                needs_redraw = true;
            }
        }
    }

    void update_state() {
        try_reconnect();
        read_serial();
        if (!paused && difftime(time(nullptr), last_save) >= save_interval) {
            save_data();
            last_save = time(nullptr);
            needs_redraw = true;
            menu_needs_redraw = true;
        }
    }

    void render() {
        set_foreground(background_color);
        XFillRectangle(dpy, pixmap, gc, 0, 0, WIDTH, HEIGHT);
        draw_graph(100, 40, 600, 200, true, 18.0f, colors[1], colors[0]);
        draw_graph(100, 290, 600, 200, false, 0.0f, colors[2], colors[3]);
        draw_footer();
        draw_errors();
        draw_help();
        x11->copy_pixmap_to_window();
        needs_redraw = false;
    }

public:
    BMP280Gui(int argc, char* argv[]) : menu_win(0), menu_gc(0), fd(-1), last_save(0) {
        XSetErrorHandler(x11_error_handler);

        try {
            x11 = std::make_unique<X11Display>();
            dpy = x11->get_display();
            win = x11->get_window();
            gc = x11->get_gc();
            pixmap = x11->get_pixmap();
        } catch (const std::exception& e) {
            std::cerr << "X11 initialization failed: " << e.what() << "\n";
            throw;
        }

        int screen = DefaultScreen(dpy);
        menu_win = XCreateSimpleWindow(dpy, win, 0, 1, WIDTH, 30, 0,
                                       BlackPixel(dpy, screen), WhitePixel(dpy, screen));
        XSelectInput(dpy, menu_win, ExposureMask | ButtonPressMask);
        XMapWindow(dpy, menu_win);
        menu_gc = XCreateGC(dpy, menu_win, 0, nullptr);
        XSetForeground(dpy, menu_gc, BlackPixel(dpy, screen));

        load_fonts();
        load_config("bmp280.ini");

        if (argc > 1) filename = argv[1];
        else {
            char timestr[32];
            time_t now = time(nullptr);
            strftime(timestr, sizeof(timestr), "data_%Y%m%d_%H%M%S.csv", localtime(&now));
            filename = timestr;
        }
        if (argc > 2) {
            int baud = std::atoi(argv[2]);
            switch (baud) {
                case 9600: baud_rate = B9600; break;
                case 115200: baud_rate = B115200; break;
                default:
                    baud_rate = B9600;
                    add_error("Unsupported baud rate: " + std::to_string(baud));
                    break;
            }
        }
        if (argc > 3 && argv[3][0]) csv_delimiter = argv[3][0];

        auto port = find_serial_port();
        if (!port) {
            add_error("No serial port found", true);
        } else if (!open_serial(*port, baud_rate)) {
            add_error("Unable to open serial port: " + *port, true);
        }

        if (load_data("logs/" + filename)) {
            add_error("Loaded data from logs/" + filename);
        }
    }

    ~BMP280Gui() {
        save_data();
        free_fonts();
        if (menu_gc) XFreeGC(dpy, menu_gc);
        if (menu_win) XDestroyWindow(dpy, menu_win);
    }

    void run() {
        GuiState current_state = {zoom_temp, zoom_press, vzoom_temp, vzoom_press, offset_temp, offset_press, static_cast<int>(theme), show_help, paused, selected_help_item, history.get_size()};
        GuiState last_state = current_state;

        while (!window_mapped) {
            XEvent evt;
            XNextEvent(dpy, &evt);
            if (evt.type == Expose) {
                window_mapped = true;
                needs_redraw = true;
                menu_needs_redraw = true;
            }
        }

        while (true) {
            handle_events();
            update_state();

            current_state = {zoom_temp, zoom_press, vzoom_temp, vzoom_press, offset_temp, offset_press, static_cast<int>(theme), show_help, paused, selected_help_item, history.get_size()};
            bool menu_highlight_changed = difftime(time(nullptr), menu_highlight_time) <= HIGHLIGHT_DURATION;

            if (needs_redraw || current_state != last_state) {
                render();
                last_state = current_state;
            }

            if (menu_needs_redraw || menu_highlight_changed) {
                draw_menu_bar();
                XFlush(dpy);
                menu_needs_redraw = false;
            }

            if (!error_messages.empty() && difftime(time(nullptr), last_error_time) > ERROR_DISPLAY_TIME) {
                error_messages.clear();
                needs_redraw = true;
            }

            struct timeval tv = {0, 200000};
            select(0, nullptr, nullptr, nullptr, &tv);
        }
    }
};

int main(int argc, char* argv[]) {
    try {
        BMP280Gui app(argc, argv);
        app.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}