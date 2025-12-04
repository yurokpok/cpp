#pragma once
#include <string>
#include <optional>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <vector>

struct AiConfig {
    std::string inference_source = "remote"; // "remote" или "local"
    std::string host;
    std::string port = "443";
    std::string api_key;
    // Локальные настройки
    std::string local_host = "127.0.0.1";
    int local_port = 8080;
    std::string local_model_path;
};

// Структура только для сохранения ответов ИИ
struct SavedResponse {
    std::string response;
    std::string timestamp;
};

class AiAgent {
public:
    AiAgent();
    ~AiAgent();

    // Загрузка конфигурации
    bool loadConfig(const std::string& path, std::string* err = nullptr);
    
    // Загрузка промпта (для обратной совместимости)
    bool loadPrompt(const std::string& path, std::string* err = nullptr);
    
    // Основные методы анализа кода
    std::optional<std::string> analyzeCodeFile(const std::string& filepath, 
                                              const std::string& language = "auto",
                                              std::string* err = nullptr);
                                              
    std::optional<std::string> analyzeCodeString(const std::string& code,
                                                const std::string& language = "auto",
                                                std::string* err = nullptr);
    
    // Интерактивный режим анализа кода
    void runInteractiveMode();
    
    // Управление контекстом (только ответы ИИ)
    bool enableContext();
    bool disableContext();
    std::vector<SavedResponse> getSavedResponses() const;
    bool clearSavedResponses();
    
    // Вспомогательные методы
    static bool readWholeFile(const std::string& path, std::string& out, std::string* err);
    void setPrompt(const std::string& p) { prompt_ = p; }

private:
    // Низкоуровневые методы запросов
    std::optional<std::string> httpsPostGenerate(const std::string& jsonBody, std::string* err);
    std::optional<std::string> sendLocalRequest(const std::string& prompt, std::string* err);
    std::optional<std::string> sendRequest(const std::string& prompt, std::string* err);
    
    // Обработка промптов
    std::string buildAnalysisPrompt(const std::string& code, 
                                   const std::string& language,
                                   bool is_complete_code = false) const;
    
    // Определение языка программирования
    std::string detectLanguage(const std::string& code) const;
    
    // Работа с SQLite (только для сохранения ответов)
    bool initResponseDatabase();
    bool saveResponse(const std::string& response);
    void closeDatabase();

private:
    AiConfig cfg_;
    std::string prompt_;
    bool context_enabled_ = false;
    sqlite3* db_ = nullptr;
    std::string db_path_ = "ai_responses.db";
};