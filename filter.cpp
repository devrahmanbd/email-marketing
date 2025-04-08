// Code Compile Command
// g++ -std=c++17 -o filter filter.cpp | For Mac
// x86_64-w64-mingw32-g++ -std=c++17 -static -static-libgcc -static-libstdc++ -o filter.exe filter.cpp | For Windows

#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <regex>
#include <cctype>

namespace fs = std::filesystem;

std::string toLower(const std::string &s) {
    std::string lower;
    lower.reserve(s.size());
    for (char c : s) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    return lower;
}

std::unordered_set<std::string> readFilterFile(const std::string &filename) {
    std::unordered_set<std::string> filters;
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "Error: Cannot open file " << filename << "\n";
        return filters;
    }
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string pattern;
        if (iss >> pattern) {
            filters.insert(toLower(pattern));
        }
    }
    return filters;
}

std::string extractDomain(const std::string &email) {
    auto atPos = email.find('@');
    if (atPos == std::string::npos || atPos + 1 >= email.size()) {
        return "";
    }
    return toLower(email.substr(atPos + 1));
}

bool containsPattern(const std::string &text, const std::unordered_set<std::string> &patterns) {
    for (const auto &pattern : patterns) {
        if (text.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void processEmailFile(const fs::path &filePath, const std::unordered_set<std::string> &filterSet, char mode, std::ofstream &outputFile) {
    std::ifstream file(filePath);
    if (!file) {
        std::cerr << "Error: Cannot open email file " << filePath << "\n";
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::string domain = extractDomain(line);
        if (domain.empty()) continue;
        if (mode == 'r') {
            if (containsPattern(domain, filterSet))
                continue;
        } else if (mode == 'c') {
            if (!containsPattern(domain, filterSet))
                continue;
        }
        outputFile << line << "\n";
    }
}

std::string wildcardToRegex(const std::string &pattern) {
    std::string regexStr;
    regexStr.reserve(pattern.size() * 2);
    regexStr.push_back('^');
    for (char ch : pattern) {
        switch (ch) {
            case '*': regexStr.append(".*"); break;
            case '?': regexStr.push_back('.'); break;
            case '.': regexStr.append("\\."); break;
            default: 
                if (std::string("()[]{}+^$\\|").find(ch) != std::string::npos)
                    regexStr.push_back('\\');
                regexStr.push_back(ch);
        }
    }
    regexStr.push_back('$');
    return regexStr;
}

int main() {
    std::cout << "Choose filter mode:\n(r) Remove emails if domain contains a filter pattern\n(c) Only include emails if domain contains a filter pattern\nEnter choice (r/c): ";
    char mode;
    std::cin >> mode;
    mode = static_cast<char>(std::tolower(mode));
    if (mode != 'r' && mode != 'c') {
        std::cerr << "Invalid mode selected. Exiting.\n";
        return 1;
    }
    std::cin.ignore();
    std::string defaultFilterFile = (mode == 'r') ? "remove.txt" : "contains.txt";
    std::cout << "Enter filter file path (press Enter for default '" << defaultFilterFile << "'): ";
    std::string filterFile;
    std::getline(std::cin, filterFile);
    if (filterFile.empty()) {
        filterFile = defaultFilterFile;
    }
    auto filterSet = readFilterFile(filterFile);
    if (filterSet.empty()) {
        std::cerr << "Warning: The filter set is empty or the file could not be read.\n";
    }
    std::cout << "Enter the file pattern to filter emails (e.g., email.txt or *.txt): ";
    std::string filePattern;
    std::getline(std::cin, filePattern);
    if (filePattern.empty()) {
        std::cerr << "No file pattern provided. Exiting.\n";
        return 1;
    }
    std::regex patternRegex(wildcardToRegex(filePattern), std::regex_constants::icase);
    std::vector<fs::path> filesToProcess;
    for (const auto &entry : fs::directory_iterator(fs::current_path())) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            if (filename == fs::path(filterFile).filename().string() || filename == "filtered_emails.txt")
                continue;
            if (std::regex_match(filename, patternRegex)) {
                filesToProcess.push_back(entry.path());
            }
        }
    }
    if (filesToProcess.empty()) {
        std::cerr << "No files matched the given pattern.\n";
        return 1;
    }
    std::ofstream outputFile("filtered_emails.txt", std::ios::app);
    if (!outputFile) {
        std::cerr << "Error: Cannot open output file filtered_emails.txt\n";
        return 1;
    }
    for (const auto &filePath : filesToProcess) {
        std::cout << "Processing file: " << filePath << "\n";
        processEmailFile(filePath, filterSet, mode, outputFile);
    }
    std::cout << "Filtering complete. Check filtered_emails.txt for results.\n";
    return 0;
}
