// ulp.cpp
// High-performance line processor with configurable filters and column conversion
// Supports std::filesystem or std::experimental::filesystem on macOS

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <queue>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

#if defined(__has_include)
  #if __has_include(<filesystem>)
    #include <filesystem>
    namespace fs = std::filesystem;
  #elif __has_include(<experimental/filesystem>)
    #include <experimental/filesystem>
    namespace fs = std::experimental::filesystem;
  #else
    #error "No filesystem support: neither <filesystem> nor <experimental/filesystem> found"
  #endif
#else
  #include <experimental/filesystem>
  namespace fs = std::experimental::filesystem;
#endif

#ifdef _WIN32
  #include <windows.h>
  #include <commdlg.h>
  #pragma comment(lib, "comdlg32.lib")
#endif

// Trim whitespace from both ends
static std::string trim(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end   = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos)
         ? ""
         : s.substr(start, end - start + 1);
}

// Convert string to lowercase
static std::string toLower(const std::string &s) {
    std::string res = s;
    std::transform(res.begin(), res.end(), res.begin(), ::tolower);
    return res;
}

// Split string by delimiter
static std::vector<std::string> split(const std::string &s, const std::string &delimiter) {
    std::vector<std::string> tokens;
    size_t start = 0, pos;
    while ((pos = s.find(delimiter, start)) != std::string::npos) {
        tokens.push_back(s.substr(start, pos - start));
        start = pos + delimiter.length();
    }
    tokens.push_back(s.substr(start));
    return tokens;
}

// Join vector of strings with delimiter
static std::string join(const std::vector<std::string> &parts, const std::string &delimiter) {
    std::ostringstream oss;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) oss << delimiter;
        oss << parts[i];
    }
    return oss.str();
}

struct Config {
    std::string separator;
    std::string format;
    std::string convert_format;
    std::unordered_set<std::string> email_remove;
    std::unordered_set<std::string> email_contains;
    std::unordered_set<std::string> url_remove;
    std::unordered_set<std::string> url_contains;
    std::string custom_filter;
};

// Parse config file
static Config parseConfig(const std::string &filename) {
    Config config;
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "Cannot open config file: " << filename << "\n";
        std::exit(1);
    }
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key   = trim(line.substr(0, pos));
        std::string value = trim(line.substr(pos + 1));
        if (key == "separator")           config.separator = value;
        else if (key == "format")         config.format = value;
        else if (key == "convert_format") config.convert_format = value;
        else if (key == "custom_filter")  config.custom_filter = value;
        else {
            auto tokens = split(value, ",");
            for (auto &t : tokens) {
                t = toLower(trim(t));
                if (key == "email_remove")   config.email_remove.insert(t);
                if (key == "email_contains") config.email_contains.insert(t);
                if (key == "url_remove")     config.url_remove.insert(t);
                if (key == "url_contains")   config.url_contains.insert(t);
            }
        }
    }
    return config;
}

// Advanced regexes for email and URL
static const std::regex advancedEmailRegex(
    R"(([\w\.-]+)@([\w\.-]+\.[a-zA-Z]{2,}))"
);
static const std::regex advancedUrlRegex(
    R"((https?://)?((?:[\w-]+\.)+[a-zA-Z]{2,})(:\d+)?(/[^\s\r\n]*)?)"
);

// Extract domain from email
static std::string extractEmailDomain(const std::string &email) {
    size_t pos = email.find('@');
    return (pos == std::string::npos)
         ? ""
         : toLower(email.substr(pos + 1));
}

// Extract domain from URL
static std::string extractUrlDomain(const std::string &url) {
    std::smatch match;
    if (std::regex_search(url, match, advancedUrlRegex) && match.size() >= 3)
        return toLower(match[2].str());
    return "";
}

// Check if domain matches a pattern (exact or subdomain)
static bool domainMatches(const std::string &domain, const std::string &pattern) {
    if (domain == pattern) return true;
    if (domain.size() > pattern.size() &&
        domain.compare(domain.size() - pattern.size(), pattern.size(), pattern) == 0 &&
        (domain.size() == pattern.size() ||
         domain[domain.size() - pattern.size() - 1] == '.'))
        return true;
    return false;
}

// Check domain against remove/contain sets
static bool checkDomain(const std::string &domain,
                        const std::unordered_set<std::string> &removeSet,
                        const std::unordered_set<std::string> &containSet) {
    for (const auto &r : removeSet)
        if (domainMatches(domain, r)) return false;
    if (!containSet.empty()) {
        for (const auto &c : containSet)
            if (domainMatches(domain, c)) return true;
        return false;
    }
    return true;
}

// Validate email
static bool isValidEmail(const std::string &s) {
    return std::regex_match(s, advancedEmailRegex);
}

// Validate phone number
static bool isPhoneNumber(const std::string &s) {
    static const std::regex phoneRegex(R"(^\+?\d{6,}$)");
    return std::regex_match(s, phoneRegex);
}

// Thread-safe queue for producer/consumer
template<typename T>
class ThreadSafeQueue {
public:
    void push(const T &item) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(item);
        cond_.notify_one();
    }
    bool pop(T &item) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.empty() && !done_) cond_.wait(lock);
        if (!queue_.empty()) {
            item = std::move(queue_.front());
            queue_.pop();
            return true;
        }
        return false;
    }
    void popBatch(std::vector<T> &batch, size_t maxBatchSize) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.empty() && !done_) cond_.wait(lock);
        while (!queue_.empty() && batch.size() < maxBatchSize) {
            batch.push_back(std::move(queue_.front()));
            queue_.pop();
        }
    }
    void setDone() {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
        cond_.notify_all();
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty;
        std::swap(queue_, empty);
        done_ = false;
    }
private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool done_ = false;
};

static std::mutex duplicate_mutex;
static std::unordered_set<std::string> global_duplicates;
static std::atomic<bool> doneReading{false};
static std::atomic<unsigned long long> processedCount{0};
static ThreadSafeQueue<std::string> inputQueue;
static ThreadSafeQueue<std::string> outputQueue;

// Convert wildcard pattern to regex
static std::regex wildcardToRegex(const std::string &pattern) {
    std::string regexPattern;
    for (char ch : pattern) {
        switch (ch) {
            case '*': regexPattern += ".*";  break;
            case '?': regexPattern += ".";   break;
            case '.': regexPattern += "\\."; break;
            default:  regexPattern.push_back(ch); break;
        }
    }
    return std::regex(regexPattern, std::regex::icase);
}

// Recursively find files matching pattern
static std::vector<std::string> getFiles(const std::string &pattern) {
    std::vector<std::string> files;
    std::regex r = wildcardToRegex(pattern);
    fs::path base = fs::current_path();
    for (auto &p : fs::recursive_directory_iterator(base)) {
        if (p.is_regular_file()) {
            std::string fname = p.path().filename().string();
            if (std::regex_match(fname, r))
                files.push_back(p.path().string());
        }
    }
    return files;
}

// Process a single line according to config
static std::string processLine(const std::string &line, const Config &config) {
    if (line.empty()) return "";
    auto tokens = split(line, config.separator);
    std::string url, login, pass;
    bool valid = false;

    // Parse based on input format
    if (config.format == "url:email:pass") {
        if (tokens.size() < 3) return "";
        login = trim(tokens[tokens.size() - 2]);
        pass  = trim(tokens[tokens.size() - 1]);
        url   = trim(join(
                    std::vector<std::string>(tokens.begin(), tokens.end() - 2),
                    config.separator));
        if (!isValidEmail(login) || isPhoneNumber(login)) return "";
        valid = true;

    } else if (config.format == "email:pass") {
        if (tokens.size() < 2) return "";
        login = trim(tokens[0]);
        pass  = trim(tokens[1]);
        if (!isValidEmail(login) || isPhoneNumber(login)) return "";
        valid = true;
    }
    if (!valid) return "";

    // Domain filtering
    std::string emailDomain = extractEmailDomain(login);
    if (!checkDomain(emailDomain, config.email_remove, config.email_contains))
        return "";
    if ((!config.url_remove.empty() || !config.url_contains.empty()) && !url.empty()) {
        std::string urlDomain = extractUrlDomain(url);
        if (!checkDomain(urlDomain, config.url_remove, config.url_contains))
            return "";
    }

    // Custom regex filter
    if (!config.custom_filter.empty()) {
        std::regex customReg(config.custom_filter, std::regex::icase);
        if (!std::regex_search(line, customReg))
            return "";
    }

    // Build output based on convert_format
    std::string output_line;
    const std::string &cf = config.convert_format;
    auto idxTokens = split(cf, config.separator);

    // Detect numeric convert_format like "1", "2:3", etc.
    bool isNumericFormat = !idxTokens.empty();
    for (auto &str : idxTokens) {
        if (str.empty() || !std::all_of(str.begin(), str.end(), ::isdigit)) {
            isNumericFormat = false;
            break;
        }
    }
    if (isNumericFormat) {
        std::vector<std::string> outParts;
        for (auto &idxStr : idxTokens) {
            int idx = std::stoi(idxStr) - 1;
            if (idx >= 0 && idx < static_cast<int>(tokens.size())) {
                outParts.push_back(trim(tokens[idx]));
            }
        }
        output_line = join(outParts, config.separator);

    // Named convert_format when format="url:email:pass"
    } else if (cf == "email:pass" && config.format == "url:email:pass") {
        output_line = login + config.separator + pass;

    // Named convert_format when format="email:pass"
    } else if (config.format == "email:pass") {
        if (cf == "email") {
            output_line = login;
        } else if (cf == "pass") {
            output_line = pass;
        } else {
            output_line = line;
        }

    } else {
        output_line = line;
    }

    return output_line;
}

// Worker thread: process batches of lines
static void worker(const Config &config) {
    std::unordered_set<std::string> localDuplicates;
    std::vector<std::string> batch;
    constexpr size_t BATCH_SIZE = 100;
    while (true) {
        batch.clear();
        inputQueue.popBatch(batch, BATCH_SIZE);
        if (batch.empty() && doneReading.load()) break;
        for (const auto &line : batch) {
            std::string processed = processLine(line, config);
            if (!processed.empty()) {
                if (!localDuplicates.insert(processed).second) continue;
                {
                    std::lock_guard<std::mutex> lock(duplicate_mutex);
                    if (!global_duplicates.insert(processed).second) continue;
                }
                outputQueue.push(processed);
                ++processedCount;
            }
        }
    }
}

// Producer thread: read input file lines
static void producer(const std::string &inputFilename) {
    std::ifstream infile(inputFilename);
    if (!infile) {
        std::cerr << "Cannot open input file: " << inputFilename << "\n";
        std::exit(1);
    }
    std::string line;
    while (std::getline(infile, line)) {
        inputQueue.push(line);
    }
    doneReading = true;
    inputQueue.setDone();
}

// Writer thread: append processed lines to output file
static void writer(const std::string &outputFilename, std::atomic<bool> &writerDone) {
    std::ofstream outfile(outputFilename, std::ios::app);
    if (!outfile) {
        std::cerr << "Cannot open output file: " << outputFilename << "\n";
        std::exit(1);
    }
    std::string processed;
    while (outputQueue.pop(processed)) {
        outfile << processed << "\n";
    }
    writerDone = true;
}

// Progress monitor: print processed line count
static void progressMonitor(std::atomic<bool> &doneProgress) {
    while (!doneProgress.load()) {
        std::cout << "\rProcessed lines: " << processedCount.load() << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cout << "\rProcessed lines: " << processedCount.load() << std::endl;
}

int main(int argc, char* argv[]) {
    Config config = parseConfig("config.ini");

    std::vector<std::string> inputFiles;
#ifdef _WIN32
    if (argc < 2) {
        extern std::vector<std::string> getFilesViaDialog();
        inputFiles = getFilesViaDialog();
        if (inputFiles.empty()) {
            std::cerr << "No file selected.\n";
            return 1;
        }
    } else {
#endif
        if (argc < 2) {
            std::cerr << "Usage: " << argv[0] << " <input_file_or_wildcard> [additional files...]\n";
            return 1;
        }
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg.find('*') != std::string::npos || arg.find('?') != std::string::npos) {
                auto found = getFiles(arg);
                inputFiles.insert(inputFiles.end(), found.begin(), found.end());
            } else {
                inputFiles.push_back(arg);
            }
        }
#ifdef _WIN32
    }
#endif

    if (inputFiles.empty()) {
        std::cerr << "No valid input files found.\n";
        return 1;
    }

    const std::string mergedOutputFile = "filtered_output.txt";
    std::remove(mergedOutputFile.c_str());
    std::atomic<bool> writerDone{false};
    std::thread writerThread(writer, mergedOutputFile, std::ref(writerDone));

    for (const auto &inputFile : inputFiles) {
        std::cout << "\nFiltering file: " << inputFile << std::endl;
        if (!fs::exists(inputFile) || fs::file_size(inputFile) == 0) continue;

        processedCount = 0;
        {
            std::lock_guard<std::mutex> lock(duplicate_mutex);
            global_duplicates.clear();
        }
        inputQueue.clear();
        doneReading = false;
        std::atomic<bool> progressDone{false};

        std::thread progressThread(progressMonitor, std::ref(progressDone));
        std::thread prodThread(producer, inputFile);

        unsigned int num_workers = std::thread::hardware_concurrency();
        if (num_workers == 0) num_workers = 4;
        std::vector<std::thread> workers;
        workers.reserve(num_workers);
        for (unsigned int i = 0; i < num_workers; ++i) {
            workers.emplace_back(worker, std::cref(config));
        }

        prodThread.join();
        for (auto &w : workers) w.join();

        progressDone = true;
        progressThread.join();
        std::cout << "Finished filtering file: " << inputFile << std::endl;
    }

    outputQueue.setDone();
    while (!writerDone.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    writerThread.join();

    std::cout << "\nOutput written to: " << mergedOutputFile << "\n";
    return 0;
}
