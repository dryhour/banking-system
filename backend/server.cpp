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

#include <mysqlx/xdevapi.h>

const int PORT = 8080;
const std::string PATH = "../pages/";

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

bool checkLogin(mysqlx::Session& session, const std::string& username, const std::string& password, const std::string& typeUser) {
    try {
        auto result = session.sql("SELECT * FROM users WHERE username=? AND password=? AND user_type=?")
            .bind(username, password, typeUser)
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

    std::string username = getValue("username");
    std::string password = getValue("password");

    if (checkLogin(session, username, password, "admin")) {
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
            "username VARCHAR(50) UNIQUE NOT NULL,"
            "password VARCHAR(255) NOT NULL,"
            "user_type VARCHAR(20) NOT NULL)"
        ).execute();

        session.sql(
            "INSERT IGNORE INTO users (username, password, user_type) "
            "VALUES ('admin', 'password123', 'admin')"
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