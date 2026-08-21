// looks through a folder of video clips and works out how far each one can be
// compressed before the quality actually suffers. encodes a short sample at a
// few different crf values, scores each with vmaf, and keeps the smallest one
// that still passes the floor.
//
// needs ffmpeg + ffprobe on PATH, and the ffmpeg build has to include libvmaf.
// check with: ffmpeg -version | findstr libvmaf
//
// build: g++ -std=c++17 -O2 -static vmaf_optimizer.cpp -o vmaf_optimizer.exe
//
// -static matters. without it the exe needs msys64's dlls on PATH at runtime and
// other installs (postgres, swipl) ship older copies of the same ones that break it.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifdef _WIN32
  #define POPEN _popen
  #define PCLOSE _pclose
  static const char* DEV_NULL = "NUL";
#else
  #define POPEN popen
  #define PCLOSE pclose
  static const char* DEV_NULL = "/dev/null";
#endif

struct Settings {
    fs::path input_folder;
    double min_vmaf = 93.0;
    std::string codec = "libx264";
    std::string preset = "medium";
    bool replace_originals = false;
    bool debug = false;
    std::vector<int> crf_values;
};

// one of these per crf value we try on the sample
struct CrfTest {
    int crf = 0;
    double vmaf_score = -1.0;
    uintmax_t sample_size = 0;
    bool valid = false;
};

// what ends up as a row in the csv
struct ClipResult {
    fs::path file;
    uintmax_t original_size = 0;
    uintmax_t new_size = 0;
    int chosen_crf = 0;
    double vmaf_score = -1.0;
    bool passed = false;
};

// set once in main. the helpers that need these sit too deep in the call stack
// to keep handing the settings object down just for logging.
static bool debug_logging = false;
static fs::path debug_log_file;

// runs a command, throws away the output, returns the exit code.
// with --debug the output goes into the log file instead.
int run_command(const std::string& command) {
    if (debug_logging) {
        std::ofstream log(debug_log_file, std::ios::app);
        log << "\n$ " << command << "\n";
        log.close();
        std::string logged = command + " >> \"" + debug_log_file.string() + "\" 2>&1";
        return std::system(logged.c_str());
    }
    std::string silenced = command + " > " + DEV_NULL + " 2>&1";
    return std::system(silenced.c_str());
}

// same thing but keeps stdout, which is all we need for ffprobe
std::string run_and_read_output(const std::string& command) {
    std::string output;
    FILE* pipe = POPEN(command.c_str(), "r");
    if (!pipe) return output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) output += buffer;
    PCLOSE(pipe);
    return output;
}

double get_video_duration(const fs::path& file) {
    std::string command = "ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 \"" + file.string() + "\"";
    std::string output = run_and_read_output(command);
    if (output.empty()) return 0.0;
    try { return std::stod(output); } catch (...) { return 0.0; }
}

bool is_video_file(const fs::path& file) {
    static const std::vector<std::string> video_extensions = {".mp4", ".mkv", ".mov", ".avi", ".webm", ".m4v"};
    std::string extension = file.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    return std::find(video_extensions.begin(), video_extensions.end(), extension) != video_extensions.end();
}

std::vector<fs::path> find_video_files(const fs::path& folder) {
    std::vector<fs::path> videos;
    for (auto& entry : fs::directory_iterator(folder)) {
        if (entry.is_regular_file() && is_video_file(entry.path())) {
            videos.push_back(entry.path());
        }
    }
    return videos;
}

double read_score_from_log(const fs::path& log_file) {
    std::ifstream file(log_file);
    if (!file) return -1.0;
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string contents = buffer.str();

    // every single frame in the log also has its own "vmaf" value, so start the
    // search at pooled_metrics or we end up reading a per-frame number instead
    size_t pooled_start = contents.find("\"pooled_metrics\"");
    if (pooled_start == std::string::npos) return -1.0;
    size_t vmaf_key = contents.find("\"vmaf\"", pooled_start);
    if (vmaf_key == std::string::npos) return -1.0;
    size_t mean_key = contents.find("\"mean\"", vmaf_key);
    if (mean_key == std::string::npos) return -1.0;
    size_t colon = contents.find(':', mean_key);
    if (colon == std::string::npos) return -1.0;

    try { return std::stod(contents.substr(colon + 1)); } catch (...) { return -1.0; }
}

double measure_vmaf(const fs::path& original, const fs::path& encoded, const fs::path& log_file) {
    // ffmpeg parses log_path as part of the filtergraph, where ':' and '\' both
    // mean something, so a windows path like C:\Users\... can't be passed in no
    // matter how you escape it. so: cd into the log folder and use a bare name.
    std::string filter = "[0:v]setpts=PTS-STARTPTS[dist];[1:v]setpts=PTS-STARTPTS[ref];[dist][ref]libvmaf=log_fmt=json:log_path=" + log_file.filename().string();
    std::string command = "ffmpeg -y -i \"" + encoded.string() + "\" -i \"" + original.string() + "\" -lavfi \"" + filter + "\" -f null -";

    fs::path previous_dir = fs::current_path();
    fs::current_path(log_file.parent_path());
    run_command(command);
    fs::current_path(previous_dir);

    return read_score_from_log(log_file);
}

// pulls a chunk out of the middle of the clip so the crf sweep isn't re-encoding
// the whole file four or five times just to pick a setting
bool extract_sample(const fs::path& source, const fs::path& destination, double duration) {
    if (duration <= 24.0) {
        std::error_code error;
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, error);
        return !error;
    }
    double start_time = duration * 0.25;
    double sample_length = 20.0;
    std::string command = "ffmpeg -y -ss " + std::to_string(start_time) + " -i \"" + source.string() + "\" -t " + std::to_string(sample_length) + " -c copy \"" + destination.string() + "\"";
    run_command(command);
    return fs::exists(destination) && fs::file_size(destination) > 0;
}

bool encode_video(const fs::path& source, const fs::path& destination, const Settings& settings, int crf) {
    std::string base = "ffmpeg -y -i \"" + source.string() + "\" -c:v " + settings.codec + " -crf " + std::to_string(crf) + " -preset " + settings.preset;

    std::string copy_audio = base + " -c:a copy \"" + destination.string() + "\"";
    if (run_command(copy_audio) == 0 && fs::exists(destination) && fs::file_size(destination) > 0) {
        return true;
    }

    // copying the audio stream doesn't always work depending on the source codec
    // and container, so re-encode it if the first attempt failed
    std::string reencode_audio = base + " -c:a aac -b:a 160k \"" + destination.string() + "\"";
    run_command(reencode_audio);
    return fs::exists(destination) && fs::file_size(destination) > 0;
}

std::vector<int> default_crf_values(const std::string& codec) {
    if (codec == "libx265") return {20, 24, 28, 32};
    return {18, 23, 28, 33};
}

// we want the smallest file that still clears the floor. if none of them clear
// it, use whichever scored highest rather than skipping the clip entirely.
CrfTest pick_best_crf(const std::vector<CrfTest>& tests, double min_vmaf) {
    const CrfTest* best = nullptr;
    for (auto& test : tests) {
        if (!test.valid) continue;
        if (test.vmaf_score >= min_vmaf && (!best || test.sample_size < best->sample_size)) {
            best = &test;
        }
    }
    if (best) return *best;

    const CrfTest* highest = nullptr;
    for (auto& test : tests) {
        if (!test.valid) continue;
        if (!highest || test.vmaf_score > highest->vmaf_score) highest = &test;
    }
    return highest ? *highest : CrfTest{};
}

ClipResult optimize_clip(const fs::path& source, const Settings& settings, const fs::path& temp_dir) {
    ClipResult result;
    result.file = source;
    result.original_size = fs::file_size(source);

    double duration = get_video_duration(source);
    fs::path original_sample = temp_dir / ("sample_orig" + source.extension().string());

    if (!extract_sample(source, original_sample, duration)) {
        std::cout << "  could not extract a sample, skipping\n";
        return result;
    }

    std::vector<CrfTest> tests;
    for (int crf : settings.crf_values) {
        fs::path encoded_sample = temp_dir / ("sample_" + std::to_string(crf) + source.extension().string());
        fs::path vmaf_log = temp_dir / ("vmaf_" + std::to_string(crf) + ".json");

        CrfTest test;
        test.crf = crf;

        if (encode_video(original_sample, encoded_sample, settings, crf)) {
            test.sample_size = fs::file_size(encoded_sample);
            test.vmaf_score = measure_vmaf(original_sample, encoded_sample, vmaf_log);
            test.valid = test.vmaf_score >= 0.0;
        }
        tests.push_back(test);

        std::cout << "  crf " << crf << " -> vmaf " << test.vmaf_score << ", sample size " << test.sample_size / 1024 << " kb\n";

        std::error_code error;
        fs::remove(encoded_sample, error);
        fs::remove(vmaf_log, error);
    }
    fs::remove(original_sample);

    CrfTest winner = pick_best_crf(tests, settings.min_vmaf);
    if (!winner.valid) {
        std::cout << "  no crf produced a usable score, skipping\n";
        return result;
    }

    result.chosen_crf = winner.crf;
    result.vmaf_score = winner.vmaf_score;
    result.passed = winner.vmaf_score >= settings.min_vmaf;

    fs::path output_folder = settings.replace_originals ? source.parent_path() : source.parent_path() / "optimized";
    fs::create_directories(output_folder);

    // when replacing we encode into the temp folder first, then swap it in
    fs::path output_file = settings.replace_originals ? temp_dir / source.filename() : output_folder / source.filename();

    if (!encode_video(source, output_file, settings, winner.crf)) {
        std::cout << "  full encode failed, skipping\n";
        return result;
    }
    result.new_size = fs::file_size(output_file);

    if (settings.replace_originals) {
        fs::path backup_folder = source.parent_path() / "_originals_backup";
        fs::create_directories(backup_folder);
        fs::rename(source, backup_folder / source.filename());
        fs::rename(output_file, source);
    }

    return result;
}

void print_usage() {
    std::cout <<
        "usage: vmaf_optimizer --folder <path> [options]\n"
        "  --min-vmaf <n>     quality floor, default 93\n"
        "  --codec <name>     libx264 or libx265, default libx264\n"
        "  --preset <name>    ffmpeg preset, default medium\n"
        "  --crf <a,b,c>      override the default crf sweep\n"
        "  --replace          shrink files in place (originals moved to _originals_backup)\n"
        "  --debug            log every ffmpeg command and its output to ffmpeg_debug.log\n";
}

std::vector<int> parse_crf_values(const std::string& text) {
    std::vector<int> values;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) values.push_back(std::stoi(item));
    return values;
}

Settings parse_command_line(int argc, char** argv) {
    Settings settings;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--folder" && i + 1 < argc) settings.input_folder = argv[++i];
        else if (arg == "--min-vmaf" && i + 1 < argc) settings.min_vmaf = std::stod(argv[++i]);
        else if (arg == "--codec" && i + 1 < argc) settings.codec = argv[++i];
        else if (arg == "--preset" && i + 1 < argc) settings.preset = argv[++i];
        else if (arg == "--crf" && i + 1 < argc) settings.crf_values = parse_crf_values(argv[++i]);
        else if (arg == "--replace") settings.replace_originals = true;
        else if (arg == "--debug") settings.debug = true;
        else if (arg == "--help") { print_usage(); std::exit(0); }
    }
    if (settings.crf_values.empty()) settings.crf_values = default_crf_values(settings.codec);
    return settings;
}

int main(int argc, char** argv) {
    std::cout << std::fixed << std::setprecision(2);

    Settings settings = parse_command_line(argc, argv);
    if (settings.input_folder.empty()) {
        print_usage();
        return 1;
    }
    if (!fs::exists(settings.input_folder)) {
        std::cout << "folder not found: " << settings.input_folder << "\n";
        return 1;
    }

    debug_logging = settings.debug;
    if (debug_logging) {
        debug_log_file = fs::current_path() / "ffmpeg_debug.log";
        std::cout << "debug log: " << debug_log_file << "\n";
    }

    fs::path temp_dir = settings.input_folder / "_vmaf_tmp";
    fs::create_directories(temp_dir);

    std::vector<fs::path> videos = find_video_files(settings.input_folder);
    std::cout << "found " << videos.size() << " clips\n\n";

    fs::path report_path = settings.input_folder / "vmaf_report.csv";
    std::ofstream report(report_path);
    report << "file,original_kb,new_kb,chosen_crf,vmaf,met_floor\n";

    uintmax_t bytes_before = 0;
    uintmax_t bytes_after = 0;

    for (auto& video : videos) {
        std::cout << video.filename().string() << "\n";
        ClipResult result = optimize_clip(video, settings, temp_dir);

        bytes_before += result.original_size;
        // clips we skipped still take up their original size
        bytes_after += (result.new_size ? result.new_size : result.original_size);

        report << "\"" << video.filename().string() << "\"," << result.original_size / 1024 << "," << result.new_size / 1024 << "," << result.chosen_crf << "," << result.vmaf_score << "," << (result.passed ? "yes" : "no") << "\n";
        std::cout << "\n";
    }

    std::error_code error;
    fs::remove_all(temp_dir, error);

    double percent_saved = bytes_before ? 100.0 * (1.0 - (double)bytes_after / bytes_before) : 0.0;
    std::cout << "done. " << bytes_before / (1024 * 1024) << " mb -> " << bytes_after / (1024 * 1024) << " mb (" << percent_saved << "% smaller)\n";
    std::cout << "report written to " << report_path << "\n";

    return 0;
}
