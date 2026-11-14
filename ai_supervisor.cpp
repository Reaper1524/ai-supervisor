// ai_supervisor.cpp - Единый файл AI Supervisor
// Компиляция: g++ ai_supervisor.cpp -o ai_supervisor -lssl -lcrypto -lcurl -std=c++17

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <ctime>
#include <random>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <openssl/sha.h>
#include <curl/curl.h>
#include <thread>

// ============================================
// КОНФИГУРАЦИЯ (ИЗМЕНИТЕ ПОД СЕБЯ)
// ============================================

// Хеш SHA-256 от пароля "Argus141126"
// Получить: echo -n "Argus141126" | sha256sum
const std::string DEVELOPER_CODE_HASH = "ВАШ_ХЕШ_СЮДА";

// Telegram Bot Token (получите у @BotFather)
const std::string TELEGRAM_BOT_TOKEN = "ВАШ_TELEGRAM_TOKEN";

// SMS API Key (sms.ru или другой сервис)
const std::string SMS_API_KEY = "ВАШ_SMS_API_KEY";

// API ключи для ИИ
const std::string OPENAI_API_KEY = "sk-ВАШ_OPENAI_KEY";
const std::string ANTHROPIC_API_KEY = "sk-ant-ВАШ_ANTHROPIC_KEY";
const std::string GOOGLE_API_KEY = "ВАШ_GOOGLE_KEY";

// Порт сервера
const int SERVER_PORT = 8080;

// ============================================
// СТРУКТУРЫ ДАННЫХ
// ============================================

struct TrustedPerson {
    std::string name;
    std::string phone;
    std::string telegram_id;
    bool can_approve;
};

struct TempCode {
    std::string code;
    time_t expires_at;
    std::string issued_to;
};

struct AIProvider {
    std::string name;
    std::string api_url;
    std::string api_key;
    std::string model;
    bool enabled;
    int priority;
};

struct PendingRequest {
    std::string prompt;
    time_t timestamp;
    int risk_level;
};

// ============================================
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ============================================

std::vector<TrustedPerson> trusted_contacts = {
    {"Иван Иванов", "+79991234567", "@ivan_ivanov", true},
    {"Мария Петрова", "+79997654321", "@maria_p", false}
};

std::vector<AIProvider> ai_providers = {
    {"GPT-4", "https://api.openai.com/v1/chat/completions", 
     OPENAI_API_KEY, "gpt-4-turbo-preview", true, 1},
    {"Claude", "https://api.anthropic.com/v1/messages", 
     ANTHROPIC_API_KEY, "claude-3-opus-20240229", true, 2},
    {"Gemini", "https://generativelanguage.googleapis.com/v1/models/gemini-pro:generateContent", 
     GOOGLE_API_KEY, "gemini-pro", true, 3}
};

std::map<std::string, TempCode> active_temp_codes;
std::vector<PendingRequest> pending_queue;
std::mutex codes_mutex;
std::mutex queue_mutex;

// ============================================
// УТИЛИТЫ
// ============================================

std::string hash_string(const std::string& input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, input.c_str(), input.size());
    SHA256_Final(hash, &sha256);
    
    std::stringstream ss;
    for(int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

std::string generate_temp_code() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(100000, 999999);
    return std::to_string(dist(gen));
}

void log_interaction(const std::string& ai_name, const std::string& prompt, const std::string& response) {
    std::ofstream log("supervisor_log.txt", std::ios::app);
    log << "[" << time(nullptr) << "] "
        << "AI: " << ai_name << "\n"
        << "PROMPT: " << prompt << "\n"
        << "RESPONSE: " << response.substr(0, 500) << "...\n"
        << "----------------------------------------\n";
}

// ============================================
// HTTP УТИЛИТЫ (через libcurl)
// ============================================

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string http_post(const std::string& url, const std::string& data, 
                      const std::map<std::string, std::string>& headers = {}) {
    CURL* curl;
    CURLcode res;
    std::string readBuffer;
    
    curl = curl_easy_init();
    if(curl) {
        struct curl_slist* chunk = NULL;
        for(const auto& h : headers) {
            std::string header = h.first + ": " + h.second;
            chunk = curl_slist_append(chunk, header.c_str());
        }
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        
        res = curl_easy_perform(curl);
        curl_slist_free_all(chunk);
        curl_easy_cleanup(curl);
    }
    
    return readBuffer;
}

// ============================================
// МОДУЛЬ АУТЕНТИФИКАЦИИ
// ============================================

bool verify_developer_code(const std::string& code) {
    std::string code_hash = hash_string(code);
    return code_hash == DEVELOPER_CODE_HASH;
}

bool verify_temp_code(const std::string& code) {
    std::lock_guard<std::mutex> lock(codes_mutex);
    
    auto it = active_temp_codes.find(code);
    if (it == active_temp_codes.end()) return false;
    
    time_t now = time(nullptr);
    if (now > it->second.expires_at) {
        active_temp_codes.erase(it);
        return false;
    }
    return true;
}

void store_temp_code(const std::string& code, const std::string& issued_to) {
    std::lock_guard<std::mutex> lock(codes_mutex);
    time_t expires_at = time(nullptr) + 86400; // 24 часа
    active_temp_codes[code] = {code, expires_at, issued_to};
}

// ============================================
// МОДУЛЬ УВЕДОМЛЕНИЙ
// ============================================

bool send_telegram_notification(const std::string& telegram_id, const std::string& message) {
    if (telegram_id.empty() || TELEGRAM_BOT_TOKEN.empty()) return false;
    
    std::string url = "https://api.telegram.org/bot" + TELEGRAM_BOT_TOKEN + "/sendMessage";
    
    std::string data = R"({"chat_id":")" + telegram_id + R"(","text":")" + message + R"("})";
    
    std::map<std::string, std::string> headers = {
        {"Content-Type", "application/json"}
    };
    
    std::string response = http_post(url, data, headers);
    return response.find("\"ok\":true") != std::string::npos;
}

bool send_sms_notification(const std::string& phone, const std::string& message) {
    if (phone.empty() || SMS_API_KEY.empty()) return false;
    
    std::string url = "https://sms.ru/sms/send";
    std::string data = "api_id=" + SMS_API_KEY + "&to=" + phone + "&msg=" + message;
    
    std::string response = http_post(url, data);
    return response.find("100") != std::string::npos;
}

void notify_all_trusted(const std::string& message) {
    for (const auto& person : trusted_contacts) {
        if (!person.telegram_id.empty()) {
            send_telegram_notification(person.telegram_id, message);
        }
        if (!person.phone.empty()) {
            send_sms_notification(person.phone, message);
        }
    }
}

void notify_developer(const std::string& message) {
    for (const auto& person : trusted_contacts) {
        if (person.can_approve && !person.telegram_id.empty()) {
            send_telegram_notification(person.telegram_id, "🚨 " + message);
            break;
        }
    }
}

// ============================================
// ИИ-СУДЬЯ (проверка через локальную LLM)
// ============================================

std::string call_local_llm(const std::string& prompt) {
    // Используем локальную LLM через llama.cpp сервер на порту 8081
    std::string url = "http://localhost:8081/v1/chat/completions";
    
    std::string data = R"({
        "model": "local-llm",
        "messages": [{"role": "user", "content": ")" + prompt + R"("}],
        "temperature": 0.1,
        "max_tokens": 100
    })";
    
    std::map<std::string, std::string> headers = {
        {"Content-Type", "application/json"}
    };
    
    std::string response = http_post(url, data, headers);
    
    // Простой парсинг JSON
    size_t pos = response.find("\"content\":\"");
    if (pos != std::string::npos) {
        pos += 11;
        size_t end_pos = response.find("\"", pos);
        if (end_pos != std::string::npos) {
            return response.substr(pos, end_pos - pos);
        }
    }
    
    return "";
}

bool is_request_dangerous(const std::string& prompt) {
    std::string judge_prompt = 
        "Ты - модератор безопасности. Оцени запрос по шкале 0-10, где 10 - максимально опасно.\n"
        "Запрос: " + prompt + "\n"
        "Ответь ТОЛЬКО числом от 0 до 10.";
    
    std::string response = call_local_llm(judge_prompt);
    
    try {
        int risk_score = std::stoi(response);
        return risk_score >= 7;
    } catch (...) {
        return false;
    }
}

bool contains_hallucinations(const std::string& prompt, const std::string& answer) {
    std::string judge_prompt = 
        "Ты - факт-чекер. Проверь ответ на галлюцинации.\n"
        "Вопрос: " + prompt + "\n"
        "Ответ ИИ: " + answer + "\n\n"
        "Есть ли в ответе выдуманные факты, неверные даты, несуществующие люди/события?\n"
        "Ответь: ДА или НЕТ.";
    
    std::string response = call_local_llm(judge_prompt);
    return response.find("ДА") != std::string::npos;
}

bool is_toxic_response(const std::string& answer) {
    std::string judge_prompt = 
        "Содержит ли следующий текст оскорбления, угрозы, призывы к насилию или дискриминацию?\n"
        "Текст: " + answer + "\n"
        "Ответь ДА или НЕТ.";
    
    std::string response = call_local_llm(judge_prompt);
    return response.find("ДА") != std::string::npos;
}

// ============================================
// ШЛЮЗ ИИ (вызов различных нейросетей)
// ============================================

AIProvider select_ai_for_task(const std::string& prompt) {
    for (auto& p : ai_providers) {
        if (p.enabled) return p;
    }
    return ai_providers[0];
}

std::string call_gpt4(const std::string& prompt, const AIProvider& provider) {
    std::string data = R"({
        "model": ")" + provider.model + R"(",
        "messages": [
            {"role": "system", "content": "Ты работаешь под контролем AI Supervisor."},
            {"role": "user", "content": ")" + prompt + R"("}
        ]
    })";
    
    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + provider.api_key},
        {"Content-Type", "application/json"}
    };
    
    std::string response = http_post(provider.api_url, data, headers);
    
    size_t pos = response.find("\"content\":\"");
    if (pos != std::string::npos) {
        pos += 11;
        size_t end_pos = response.find("\"", pos);
        if (end_pos != std::string::npos) {
            return response.substr(pos, end_pos - pos);
        }
    }
    
    return "Ошибка при обращении к GPT-4";
}

std::string call_claude(const std::string& prompt, const AIProvider& provider) {
    std::string data = R"({
        "model": ")" + provider.model + R"(",
        "max_tokens": 4096,
        "messages": [
            {"role": "user", "content": ")" + prompt + R"("}
        ]
    })";
    
    std::map<std::string, std::string> headers = {
        {"x-api-key", provider.api_key},
        {"anthropic-version", "2023-06-01"},
        {"Content-Type", "application/json"}
    };
    
    std::string response = http_post(provider.api_url, data, headers);
    
    size_t pos = response.find("\"text\":\"");
    if (pos != std::string::npos) {
        pos += 8;
        size_t end_pos = response.find("\"", pos);
        if (end_pos != std::string::npos) {
            return response.substr(pos, end_pos - pos);
        }
    }
    
    return "Ошибка при обращении к Claude";
}

std::string call_gemini(const std::string& prompt, const AIProvider& provider) {
    std::string url = provider.api_url + "?key=" + provider.api_key;
    
    std::string data = R"({
        "contents": [
            {"parts": [{"text": ")" + prompt + R"("}]}
        ]
    })";
    
    std::map<std::string, std::string> headers = {
        {"Content-Type", "application/json"}
    };
    
    std::string response = http_post(url, data, headers);
    
    size_t pos = response.find("\"text\":\"");
    if (pos != std::string::npos) {
        pos += 8;
        size_t end_pos = response.find("\"", pos);
        if (end_pos != std::string::npos) {
            return response.substr(pos, end_pos - pos);
        }
    }
    
    return "Ошибка при обращении к Gemini";
}

std::string call_any_ai(const std::string& prompt, const AIProvider& provider) {
    std::string response;
    
    if (provider.name == "GPT-4") {
        response = call_gpt4(prompt, provider);
    } else if (provider.name == "Claude") {
        response = call_claude(prompt, provider);
    } else if (provider.name == "Gemini") {
        response = call_gemini(prompt, provider);
    }
    
    log_interaction(provider.name, prompt, response);
    return response;
}

// ============================================
// HTTP СЕРВЕР (простая реализация)
// ============================================

std::string get_http_body(const std::string& request) {
    size_t pos = request.find("\r\n\r\n");
    if (pos != std::string::npos) {
        return request.substr(pos + 4);
    }
    return "";
}

std::string get_http_method(const std::string& request) {
    size_t pos = request.find(" ");
    if (pos != std::string::npos) {
        return request.substr(0, pos);
    }
    return "";
}

std::string get_http_path(const std::string& request) {
    size_t start = request.find(" ") + 1;
    size_t end = request.find(" ", start);
    if (start != std::string::npos && end != std::string::npos) {
        return request.substr(start, end - start);
    }
    return "";
}

std::string create_http_response(int status_code, const std::string& content_type, const std::string& body) {
    std::string status_text = (status_code == 200) ? "OK" : 
                              (status_code == 401) ? "Unauthorized" :
                              (status_code == 403) ? "Forbidden" :
                              (status_code == 404) ? "Not Found" : "Error";
    
    std::stringstream ss;
    ss << "HTTP/1.1 " << status_code << " " << status_text << "\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n"
       << "\r\n"
       << body;
    
    return ss.str();
}

// ============================================
// ОБРАБОТЧИКИ ЗАПРОСОВ
// ============================================

std::string handle_auth(const std::string& body) {
    // Простой парсинг JSON
    size_t pos = body.find("\"code\":\"");
    if (pos == std::string::npos) return create_http_response(400, "application/json", R"({"error":"Invalid request"})");
    
    pos += 8;
    size_t end_pos = body.find("\"", pos);
    std::string code = body.substr(pos, end_pos - pos);
    
    if (verify_developer_code(code)) {
        return create_http_response(200, "application/json", 
            R"({"status":"success","role":"developer","message":"Доступ разработчика подтвержден."})");
    } 
    else if (verify_temp_code(code)) {
        return create_http_response(200, "application/json", 
            R"({"status":"success","role":"operator","message":"Временный доступ подтвержден."})");
    } 
    else {
        return create_http_response(401, "application/json", 
            R"({"status":"error","message":"Неверный код доступа."})");
    }
}

std::string handle_request_temp_code(const std::string& body) {
    size_t pos = body.find("\"developer_code\":\"");
    if (pos == std::string::npos) return create_http_response(400, "application/json", R"({"error":"Invalid request"})");
    
    pos += 18;
    size_t end_pos = body.find("\"", pos);
    std::string dev_code = body.substr(pos, end_pos - pos);
    
    if (!verify_developer_code(dev_code)) {
        return create_http_response(403, "application/json", 
            R"({"status":"error","message":"Только разработчик может запрашивать временные коды."})");
    }
    
    std::string temp_code = generate_temp_code();
    store_temp_code(temp_code, "developer");
    
    std::string message = "🔐 Временный код доступа: " + temp_code + "\nДействителен 24 часа.";
    notify_all_trusted(message);
    
    return create_http_response(200, "application/json", 
        R"({"status":"success","message":"Временный код отправлен доверенным лицам.","code":")" + temp_code + R"("})");
}

std::string handle_ask(const std::string& body) {
    size_t pos = body.find("\"prompt\":\"");
    if (pos == std::string::npos) return create_http_response(400, "application/json", R"({"error":"Invalid request"})");
    
    pos += 10;
    size_t end_pos = body.find("\"", pos);
    std::string prompt = body.substr(pos, end_pos - pos);
    
    // Проверка на опасность
    if (is_request_dangerous(prompt)) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        pending_queue.push_back({prompt, time(nullptr), 9});
        notify_developer("⚠️ Опасный запрос заблокирован: " + prompt);
        return create_http_response(200, "application/json", 
            R"({"status":"pending_human_approval","message":"Запрос требует подтверждения оператора."})");
    }
    
    // Вызов ИИ
    AIProvider provider = select_ai_for_task(prompt);
    std::string ai_response = call_any_ai(prompt, provider);
    
    // Проверка на галлюцинации
    if (contains_hallucinations(prompt, ai_response)) {
        notify_developer("🤥 Обнаружены галлюцинации в ответе на: " + prompt);
        return create_http_response(200, "application/json", 
            R"({"status":"hallucination_detected","message":"Ответ содержит недостоверную информацию."})");
    }
    
    // Проверка на токсичность
    if (is_toxic_response(ai_response)) {
        return create_http_response(200, "application/json", 
            R"({"status":"toxic_blocked","message":"Ответ содержит недопустимый контент."})");
    }
    
    // Успех
    return create_http_response(200, "application/json", 
        R"({"status":"success","response":")" + ai_response + R"("})");
}

std::string handle_pending() {
    std::lock_guard<std::mutex> lock(queue_mutex);
    std::string json = R"({"queue":[)";
    
    for (size_t i = 0; i < pending_queue.size(); i++) {
        if (i > 0) json += ",";
        json += R"({"prompt":")" + pending_queue[i].prompt + R"(","timestamp":)" + 
                std::to_string(pending_queue[i].timestamp) + R"(,"risk_level":)" + 
                std::to_string(pending_queue[i].risk_level) + "}";
    }
    
    json += "]}";
    return create_http_response(200, "application/json", json);
}

std::string handle_approve(const std::string& body) {
    size_t pos = body.find("\"index\":");
    if (pos == std::string::npos) return create_http_response(400, "application/json", R"({"error":"Invalid request"})");
    
    pos += 8;
    size_t end_pos = body.find(",", pos);
    if (end_pos == std::string::npos) end_pos = body.find("}", pos);
    std::string index_str = body.substr(pos, end_pos - pos);
    
    try {
        int index = std::stoi(index_str);
        
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (index >= 0 && index < pending_queue.size()) {
            std::string prompt = pending_queue[index].prompt;
            pending_queue.erase(pending_queue.begin() + index);
            
            AIProvider provider = select_ai_for_task(prompt);
            std::string ai_response = call_any_ai(prompt, provider);
            
            return create_http_response(200, "application/json", 
                R"({"status":"approved","response":")" + ai_response + R"("})");
        }
    } catch (...) {}
    
    return create_http_response(404, "text/plain", "Request not found");
}

std::string handle_index() {
    std::string html = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>AI Supervisor</title>
    <style>
        body { font-family: Arial, sans-serif; max-width: 800px; margin: 50px auto; padding: 20px; }
        .section { margin: 20px 0; padding: 20px; border: 1px solid #ddd; border-radius: 5px; }
        input, textarea, button { padding: 10px; margin: 5px 0; width: 100%; }
        button { background: #007bff; color: white; border: none; cursor: pointer; }
        button:hover { background: #0056b3; }
        .result { background: #f5f5f5; padding: 10px; margin: 10px 0; border-radius: 5px; }
    </style>
</head>
<body>
    <h1>🤖 AI Supervisor</h1>
    
    <div class="section">
        <h2>🔐 Вход</h2>
        <input type="text" id="code" placeholder="Код доступа">
        <button onclick="login()">Войти</button>
        <div id="loginResult" class="result"></div>
    </div>
    
    <div class="section">
        <h2>📤 Запрос к ИИ</h2>
        <textarea id="prompt" rows="4" placeholder="Введите запрос"></textarea>
        <button onclick="askAI()">Отправить</button>
        <div id="askResult" class="result"></div>
    </div>
    
    <div class="section">
        <h2>⏳ Очередь на одобрение</h2>
        <button onclick="loadQueue()">Обновить очередь</button>
        <div id="queue"></div>
    </div>
    
    <script>
        async function login() {
            const res = await fetch('/auth', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({code: document.getElementById('code').value})
            });
            const data = await res.json();
            document.getElementById('loginResult').innerText = JSON.stringify(data, null, 2);
        }
        
        async function askAI() {
            const res = await fetch('/ask', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({prompt: document.getElementById('prompt').value})
            });
            const data = await res.json();
            document.getElementById('askResult').innerText = JSON.stringify(data, null, 2);
        }
        
        async function loadQueue() {
            const res = await fetch('/pending');
            const data = await res.json();
            let html = '<ul>';
            data.queue.forEach((item, index) => {
                html += '<li>' + item.prompt + ' <button onclick="approve(' + index + ')">Одобрить</button></li>';
            });
            html += '</ul>';
            document.getElementById('queue').innerHTML = html;
        }
        
        async function approve(index) {
            const res = await fetch('/approve', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({index: index})
            });
            const data = await res.json();
            alert(JSON.stringify(data, null, 2));
            loadQueue();
        }
    </script>
</body>
</html>)";
    
    return create_http_response(200, "text/html", html);
}

// ============================================
// ГЛАВНАЯ ФУНКЦИЯ
// ============================================

void handle_client(int client_socket) {
    char buffer[4096];
    int bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
    
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        std::string request(buffer);
        
        std::string method = get_http_method(request);
        std::string path = get_http_path(request);
        std::string body = get_http_body(request);
        
        std::string response;
        
        if (path == "/" || path == "/index.html") {
            response = handle_index();
        }
        else if (method == "POST" && path == "/auth") {
            response = handle_auth(body);
        }
        else if (method == "POST" && path == "/request_temp_code") {
            response = handle_request_temp_code(body);
        }
        else if (method == "POST" && path == "/ask") {
            response = handle_ask(body);
        }
        else if (method == "GET" && path == "/pending") {
            response = handle_pending();
        }
        else if (method == "POST" && path == "/approve") {
            response = handle_approve(body);
        }
        else {
            response = create_http_response(404, "text/plain", "Not Found");
        }
        
        write(client_socket, response.c_str(), response.size());
    }
    
    close(client_socket);
}

int main() {
    std::cout << "=== AI SUPERVISOR ===\n";
    std::cout << "Инициализация системы...\n";
    
    // Инициализация CURL
    curl_global_init(CURL_GLOBAL_ALL);
    
    // Создание серверного сокета
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        std::cerr << "Ошибка создания сокета\n";
        return 1;
    }
    
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Ошибка привязки к порту " << SERVER_PORT << "\n";
        return 1;
    }
    
    if (listen(server_socket, 10) < 0) {
        std::cerr << "Ошибка прослушивания\n";
        return 1;
    }
    
    std::cout << "AI Supervisor запущен на порту " << SERVER_PORT << "...\n";
    std::cout << "Откройте http://localhost:" << SERVER_PORT << " в браузере\n";
    
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_socket < 0) {
            std::cerr << "Ошибка принятия соединения\n";
            continue;
        }
        
        std::thread(handle_client, client_socket).detach();
    }
    
    close(server_socket);
    curl_global_cleanup();
    
    return 0;
}
