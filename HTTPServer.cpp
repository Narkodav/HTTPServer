#include "HTTPServer.h"

void HTTPServer::stop() {
    if (!m_isRunning) {
        return;
    }

    // Stop accepting new connections
    boost::system::error_code ec;
    m_acceptor.close(ec);

    // Stop the io_context
    m_ioContext.stop();

    // If we're running non-blocking, wait for the thread to finish
    if (m_ioLoopThread.joinable()) {
        m_ioLoopThread.join();
    }

    m_isRunning = false;
}

// Start accepting connections
void HTTPServer::startNonBlocking() {
    if (m_isRunning) {
        throw std::runtime_error("Server is already running");
    }

    m_isRunning = true;
    m_ioLoopThread = boost::thread([this]() {
        try {
            accept();
            m_ioContext.run();
        }
        catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(iostreamMutex);
            std::cerr << "Server error: " << e.what() << std::endl;
        }
        m_isRunning = false;
        });
}

// Start accepting connections blocking
void HTTPServer::startBlocking() {
    if (m_isRunning) {
        throw std::runtime_error("Server is already running");
    }

    m_isRunning = true;
    try {
        accept();
        m_ioContext.run();
    }
    catch (const std::exception& e) {
        m_isRunning = false;
        throw;
    }
    m_isRunning = false;
}

void HTTPServer::accept() {
    m_acceptor.async_accept(
        [this](boost::system::error_code ec, ip::tcp::socket socket) {
            // Start accepting the next connection before processing this one
            accept();

            if (!ec) {
                // Process the connection
                processRequest(std::move(socket));
            }
            else {
                std::lock_guard<std::mutex> lock(iostreamMutex);
                std::cout << "Accept error: "
                    << "Error code: " << ec.value()
                    << ", Message: " << ec.message() << std::endl;
            }
        });
}

void HTTPServer::processRequest(ip::tcp::socket socket) {
    auto request = std::make_shared<http::request<http::string_body>>();
    auto buffer = std::make_shared<boost::beast::flat_buffer>();
    auto socket_ptr = std::make_shared<ip::tcp::socket>(std::move(socket));

    // Read the request asynchronously
    http::async_read(*socket_ptr,
        *buffer,
        *request,
        [this, request, buffer, socket_ptr]
    (boost::system::error_code ec, std::size_t bytes_transferred) mutable {
        {
            std::lock_guard<std::mutex> lock(iostreamMutex);
            std::cout << "Starting to process new request..." << std::endl;
        }
            if (!ec) {
                {
                    std::lock_guard<std::mutex> lock(iostreamMutex);
                    std::cout << "Request received: "
                        << "\nMethod: " << request->method_string()
                        << "\nTarget: " << request->target()
                        << "\nVersion: " << request->version()
                        << std::endl;
                }
                handleRequest(*request, *socket_ptr);
            }
            else
            {
                {
                    std::lock_guard<std::mutex> lock(iostreamMutex);
                    std::cout << "Error during async_read: "
                        << "\nError code: " << ec.value()
                        << "\nCategory: " << ec.category().name()
                        << "\nMessage: " << ec.message()
                        << "\nBytes transferred: " << bytes_transferred
                        << std::endl;

                    // Print the raw buffer content for debugging
                    auto data = static_cast<const unsigned char*>(buffer->data().data());
                    auto size = buffer->data().size();
                    std::cout << "Raw request data (" << size << " bytes): ";
                    for (std::size_t i = 0; i < size && i < 100; ++i) {
                        std::cout << std::hex << (int)data[i] << " ";
                    }
                    std::cout << std::dec << std::endl;
                }

                if (socket_ptr->is_open()) {
                    // Send a 400 Bad Request response
                    http::response<http::string_body> response;
                    response.version(11);  // HTTP/1.1
                    response.result(http::status::bad_request);
                    response.set(http::field::server, "Boost HTTP Server");
                    response.set(http::field::content_type, "text/plain");
                    response.body() = "400 Bad Request\n";
                    response.prepare_payload();

                    try {
                        http::write(*socket_ptr, response);
                    }
                    catch (const std::exception& e) {
                        std::cerr << "Error sending error response: " << e.what() << std::endl;
                    }

                    boost::system::error_code shutdown_ec;
                    socket_ptr->shutdown(ip::tcp::socket::shutdown_both, shutdown_ec);
                }
            }
        });
}

std::string HTTPServer::mimeType(const std::string& path) {
    size_t dot_pos = path.find_last_of('.');
    if (dot_pos != std::string::npos) {
        std::string ext = path.substr(dot_pos);
        auto it = mimeMap.find(ext);
        if (it != mimeMap.end()) {
            return it->second;
        }
    }
    return "application/octet-stream";
}

void HTTPServer::serveFile(const std::string& filepath,
    const http::request<http::string_body>& req,
    ip::tcp::socket& socket) {
    try {
        http::response<http::file_body> res;
        res.version(req.version());
        res.result(http::status::ok);
        res.set(http::field::server, "Boost HTTP Server");
        res.set(http::field::content_type, mimeType(filepath));

        boost::beast::error_code ec;
        http::file_body::value_type body;
        body.open(filepath.c_str(), boost::beast::file_mode::scan, ec);

        if (ec) {
            {
                std::lock_guard<std::mutex> lock(iostreamMutex);
                std::cout << "Error opening file: " << filepath << std::endl;
            }
            sendErrorResponse(req, socket, http::status::not_found,
                "File not found\n");
            return;
        }

        res.content_length(body.size());
        res.body() = std::move(body);
        {
            std::lock_guard<std::mutex> lock(iostreamMutex);
            std::cout << "Sending response file " << filepath << std::endl;
        }
        http::write(socket, res, ec);
        if (ec) {
            throw std::runtime_error("Error writing response: " + ec.message());
        }
    }
    catch (const std::exception& e) {
        sendErrorResponse(req, socket, http::status::internal_server_error,
            "Internal Server Error\n");
    }
}

void HTTPServer::sendErrorResponse(const http::request<http::string_body>& req,
    ip::tcp::socket& socket,
    http::status status,
    const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(iostreamMutex);
            std::cout << "Sending an error response: " << status << std::endl;
        }
    http::response<http::string_body> res;
    res.version(req.version());
    res.result(status);
    res.set(http::field::server, "Boost HTTP Server");
    res.set(http::field::content_type, "text/plain");
    res.body() = message;
    res.prepare_payload();
    http::write(socket, res);
}

void HTTPServer::handleRequest(http::request<http::string_body>& req, ip::tcp::socket& socket) {
    // Get the target path
    std::string target = std::string(req.target());
    if (target == "/") {
        target = "/index.html";
    }

    // Remove any .. to prevent directory traversal attacks
    if (target.find("..") != std::string::npos) {
        sendErrorResponse(req, socket, http::status::forbidden,
            "Forbidden\n");
        return;
    }

    // Serve the file
    serveFile("public" + target, req, socket);

    // Shutdown the socket
    boost::system::error_code ec;
    socket.shutdown(ip::tcp::socket::shutdown_send, ec);
}