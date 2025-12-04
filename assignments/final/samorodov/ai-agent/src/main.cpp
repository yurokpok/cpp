#include "AiAgent.h"
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

void printUsage() {
    std::cout << "AI Code Analyzer - Анализ кода на C++ и Python\n\n";
    std::cout << "Использование:\n";
    std::cout << "  ./ai_agent analyze <файл> [язык]   - Анализ файла\n";
    std::cout << "  ./ai_agent code \"<код>\" [язык]     - Анализ кода из строки\n";
    std::cout << "  ./ai_agent interactive             - Интерактивный режим\n";
    std::cout << "  ./ai_agent saved                   - Показать сохраненные ответы\n";
    std::cout << "  ./ai_agent clear                   - Очистить сохраненные ответы\n";
    std::cout << "  ./ai_agent help                    - Показать справку\n\n";
    
    std::cout << "Параметры:\n";
    std::cout << "  язык: cpp, python, auto (определить автоматически)\n\n";
    
    std::cout << "Примеры:\n";
    std::cout << "  ./ai_agent analyze main.cpp\n";
    std::cout << "  ./ai_agent analyze script.py python\n";
    std::cout << "  ./ai_agent code \"def test(): return 1\" python\n";
    std::cout << "  ./ai_agent interactive\n";
}

bool fileExists(const std::string& path) {
    return fs::exists(path);
}

int main(int argc, char* argv[]) {
    AiAgent agent;
    
    // Ищем config.json в разных местах
    std::string configPath;
    std::vector<std::string> possiblePaths = {
        "config.json",
        "../config.json",
        "../../config.json",
        "./config.json"
    };
    
    bool configFound = false;
    for (const auto& path : possiblePaths) {
        if (fs::exists(path)) {
            configPath = path;
            configFound = true;
            break;
        }
    }
    
    if (!configFound) {
        std::cerr << "Ошибка: Не найден config.json\n";
        std::cerr << "Создайте config.json в текущей директории\n";
        return 1;
    }
    
    std::string err;
    if (!agent.loadConfig(configPath, &err)) {
        std::cerr << "Ошибка загрузки конфига: " << err << "\n";
        return 1;
    }
    
    if (argc < 2) {
        printUsage();
        return 1;
    }
    
    std::string command = argv[1];
    
    if (command == "analyze" && argc >= 3) {
        std::string filename = argv[2];
        std::string language = (argc >= 4) ? argv[3] : "auto";
        
        if (!fileExists(filename)) {
            std::cerr << "Файл не найден: " << filename << "\n";
            return 1;
        }
        
        auto result = agent.analyzeCodeFile(filename, language, &err);
        if (!result) {
            std::cerr << "Ошибка анализа: " << err << "\n";
            return 1;
        }
        
        std::cout << *result << "\n";
        
    } else if (command == "code" && argc >= 3) {
        std::string code = argv[2];
        std::string language = (argc >= 4) ? argv[3] : "auto";
        
        auto result = agent.analyzeCodeString(code, language, &err);
        if (!result) {
            std::cerr << "Ошибка анализа: " << err << "\n";
            return 1;
        }
        
        std::cout << *result << "\n";
        
    } else if (command == "interactive") {
        agent.runInteractiveMode();
        
    } else if (command == "saved") {
        auto responses = agent.getSavedResponses();
        if (responses.empty()) {
            std::cout << "Нет сохраненных ответов\n";
        } else {
            std::cout << "=== СОХРАНЕННЫЕ ОТВЕТЫ (" << responses.size() << ") ===\n";
            for (size_t i = 0; i < responses.size(); ++i) {
                std::cout << "[" << (i+1) << "] " << responses[i].timestamp << "\n";
                std::cout << responses[i].response << "\n---\n";
            }
        }
        
    } else if (command == "clear") {
        if (agent.clearSavedResponses()) {
            std::cout << "✓ Ответы очищены\n";
        } else {
            std::cout << "✗ Ошибка очистки\n";
        }
        
    } else if (command == "help" || command == "--help" || command == "-h") {
        printUsage();
        
    } else {
        std::cerr << "Неизвестная команда: " << command << "\n";
        printUsage();
        return 1;
    }
    
    return 0;
}