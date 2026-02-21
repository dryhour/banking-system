#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/stat.h>
#include <thread>
#include <chrono>
#include <algorithm>

#include <mysqlx/xdevapi.h>

#include <random>
#include <set>

const int PORT = 8080;
const std::string PATH = "../pages/";

std::map<std::string, std::pair<std::string, std::string>> activeSessions;
std::string generateToken() {
    const std::string availableCharacters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string generatedString;

    std::random_device seed;
    std::mt19937 gen{seed()};
    std::uniform_int_distribution<int> dist(0, availableCharacters.length() - 1);

    for (int i = 0; i < 32; i++) {
        generatedString += availableCharacters[dist(gen)];  
    }

    return generatedString;
}

time_t getFileModTime(const std::string& filename) {
    struct stat fileInfo;
    if (stat(filename.c_str(), &fileInfo) != 0) {
        return 0;
    }
    return fileInfo.st_mtime;
}

std::string readFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string getRequestedFile(const std::string& request) {
    size_t start = request.find("GET ") + 4;
    size_t end = request.find(" ", start);
    std::string path = request.substr(start, end - start);
    
    if (path[0] == '/') {
        path = path.substr(1);
    }
    
    size_t queryPos = path.find('?');
    if (queryPos != std::string::npos) {
        path = path.substr(0, queryPos);
    }
    
    if (path.empty()) {
        return PATH + "index.html";
    }
    
    return PATH + path;
}

std::string injectAutoReload(const std::string& html) {
    std::string script = R"(
<script>
let lastCheck = Date.now();
setInterval(() => {
    fetch('/check?t=' + lastCheck)
        .then(response => response.text())
        .then(data => {
            if (data === 'reload') {
                location.reload();
            }
        });
    lastCheck = Date.now();
}, 1000);
</script>
</body>)";
    
    std::string modifiedHtml = html;
    size_t pos = modifiedHtml.find("</body>");
    if (pos != std::string::npos) {
        modifiedHtml.insert(pos, script);
    }
    return modifiedHtml;
}

std::string createHttpResponse(const std::string& content, const std::string& contentType = "text/html") {
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: " << contentType << "\r\n";
    response << "Content-Length: " << content.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << content;
    return response.str();
}

bool checkLogin(mysqlx::Session& session, const std::string& account_number, const std::string& pin, const std::string& typeUser) {
    try {
        auto result = session.sql("SELECT * FROM users WHERE account_number=? AND pin=? AND user_type=?")
            .bind(account_number, pin, typeUser)
            .execute();
        
        return result.count() > 0;
    } catch (const mysqlx::Error &err) {
        std::cerr << "SQL error: " << err << std::endl;
        return false;
    }
}

std::string urlDecode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.length(); i++) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int value;
            std::istringstream is(str.substr(i + 1, 2));
            if (is >> std::hex >> value) {
                result += static_cast<char>(value);
                i += 2;
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

std::string getUsername(const std::string& body) {
    auto getValue = [&](const std::string& key) {
        size_t pos = body.find(key + "=");
        if (pos == std::string::npos) return std::string("");
        size_t start = pos + key.length() + 1;
        size_t end = body.find('&', start);
        return urlDecode(body.substr(start, end == std::string::npos ? end : end - start));
    };

    std::string account_number = getValue("account_number");
    return account_number;
}

std::string handleLogin(mysqlx::Session& session, const std::string& request) {
    size_t bodyStart = request.find("\r\n\r\n");
    std::string body = (bodyStart != std::string::npos) ? request.substr(bodyStart + 4) : "";

    auto getValue = [&](const std::string& key) {
        size_t pos = body.find(key + "=");
        if (pos == std::string::npos) return std::string("");
        size_t start = pos + key.length() + 1;
        size_t end = body.find('&', start);
        return urlDecode(body.substr(start, end == std::string::npos ? end : end - start));
    };

    std::string account_number = getValue("account_number");
    std::string pin = getValue("pin");
    std::string user_type = getValue("user_type");

    if (checkLogin(session, account_number, pin, user_type)) {
        std::string token = generateToken();
        activeSessions[token] = {account_number, user_type};
        return token;
    }
    return "fail";
}

bool checkAdminFunction(
    mysqlx::Session& session, 
    const std::string& account_number, 
    const std::string& pin, 
    const std::string& type,
    const std::string& newPin
) {
    try {
        auto result = session.sql("SELECT * FROM users WHERE account_number=? AND pin=? AND user_type=?")
            .bind(account_number, pin, "user")
            .execute();

        std::string lowerUsername = account_number;
        std::transform(lowerUsername.begin(), lowerUsername.end(), lowerUsername.begin(), ::tolower);

        if (lowerUsername == "admin") {
            return false;
        }
        
        bool userExists = result.count() > 0;
        if (type == "open" and not userExists) {
            session.sql(
                "INSERT IGNORE INTO users (account_number, pin, user_type) "
                "VALUES (?, ?, 'user')"
            ).bind(account_number, pin).execute();
            return true;
        } else if (type == "close" and userExists) {
            session.sql(
                "DELETE FROM users WHERE account_number=?"
            ).bind(account_number)
            .execute();
            return true;
        } else if (type == "modify" and userExists) {
            session.sql(
                "UPDATE users SET pin = ? WHERE account_number=?"
            ).bind(newPin, account_number)
            .execute();
            return true;
        }
        return false;
    } catch (const mysqlx::Error &err) {
        std::cerr << "SQL error: " << err << std::endl;
        return false;
    }
}

std::string handleAdminFunction(mysqlx::Session& session, const std::string& request) {
    size_t bodyStart = request.find("\r\n\r\n");
    std::string body = (bodyStart != std::string::npos) ? request.substr(bodyStart + 4) : "";

    auto getValue = [&](const std::string& key) {
        size_t pos = body.find(key + "=");
        if (pos == std::string::npos) return std::string("");
        size_t start = pos + key.length() + 1;
        size_t end = body.find('&', start);
        return urlDecode(body.substr(start, end == std::string::npos ? end : end - start));
    };

    std::string account_number = getValue("account_number");
    std::string pin = getValue("pin");
    std::string type = getValue("type");
    std::string newPin = getValue("newPin");

    if (checkAdminFunction(session, account_number, pin, type, newPin)) {
        return "success";
    }
    return "fail";
}

int main() {
    try {
        mysqlx::Session session("localhost", 33060, "root", "", "banking_users");
        
        std::cout << "Connected to MySQL!" << std::endl;

        session.sql(
            "CREATE TABLE IF NOT EXISTS users ("
            "id INT AUTO_INCREMENT PRIMARY KEY,"
            "account_number VARCHAR(50) UNIQUE NOT NULL,"
            "pin VARCHAR(10) NOT NULL,"
            "user_type VARCHAR(20) NOT NULL,"
            "balance DECIMAL(10,2) NOT NULL DEFAULT 0.00)"
        ).execute();

        session.sql(
                "INSERT IGNORE INTO users (account_number, pin, user_type) "
                "VALUES (1, 1234, 'admin')"
        ).execute();

        int server_fd, client_fd;
        struct sockaddr_in address;
        int addrlen = sizeof(address);
        
        if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
            std::cerr << "Socket creation failed" << std::endl;
            return -1;
        }
        
        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
            std::cerr << "Setsockopt failed" << std::endl;
            return -1;
        }
        
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(PORT);
        
        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            std::cerr << "Bind failed" << std::endl;
            return -1;
        }
        
        if (listen(server_fd, 3) < 0) {
            std::cerr << "Listen failed" << std::endl;
            return -1;
        }
        
        while (true) {
            if ((client_fd = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
                std::cerr << "Accept failed" << std::endl;
                continue;
            }
            
            char buffer[30000] = {0};
            read(client_fd, buffer, 30000);
            
            std::string request(buffer);
            std::string response;
            
            if (request.find("GET /check") != std::string::npos) {
                response = createHttpResponse("ok", "text/plain");
            } else if (request.find("POST /login") != std::string::npos) {
                std::string result = handleLogin(session, request);
                response = createHttpResponse(result, "text/plain");
            } else if (request.find("POST /adminFunctions") != std::string::npos) {
                std::string result = handleAdminFunction(session, request);
                response = createHttpResponse(result, "text/plain");
            } else if (request.find("GET /verify") != std::string::npos) {
                size_t pos = request.find("token=");
                if (pos != std::string::npos) {
                    size_t start = pos + 6;
                    size_t end = request.find("&", start);
                    if (end == std::string::npos) end = request.find(" ", start);
                    std::string token = request.substr(start, end - start);
                    
                    auto it = activeSessions.find(token);
                    if (it != activeSessions.end()) {
                        std::string responseData = it->second.first + "&" + it->second.second;
                        response = createHttpResponse(responseData, "text/plain");
                    } else {
                        response = createHttpResponse("invalid", "text/plain");
                    }
                } else {
                    response = createHttpResponse("invalid", "text/plain");
                }
            } else {
                std::string requestedFile = getRequestedFile(request);
                
                std::string htmlContent = readFile(requestedFile);
                if (htmlContent.empty()) {
                    htmlContent = "<html><body><h1>404 - File not found: " + requestedFile + "</h1></body></html>";
                } else {
                    htmlContent = injectAutoReload(htmlContent);
                }
                response = createHttpResponse(htmlContent);
            }
            
            send(client_fd, response.c_str(), response.length(), 0);
            close(client_fd);
        }
        
        close(server_fd);
        return 0;
    } catch (const mysqlx::Error &err) {
        std::cerr << "MySQL error: " << err << std::endl;
        return -1;
    }
}