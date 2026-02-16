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

int main() {
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
}