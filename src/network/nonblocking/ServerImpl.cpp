#include "ServerImpl.h"
using namespace std;
namespace Afina {
namespace Network {
namespace NonBlocking {

ServerImpl::ServerImpl(std::shared_ptr<Afina::Storage> ps) : Server(ps) {}

ServerImpl::~ServerImpl() {}

void ServerImpl::Start(uint32_t port, uint16_t n_workers) {
    sigset_t sig_mask;
    sigemptyset(&sig_mask);
    sigaddset(&sig_mask, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &sig_mask, NULL) != 0) throw runtime_error("Unable to mask signals");
    if (static_cast<uint16_t>(port) == port) listen_port = static_cast<uint16_t>(port);
    else throw overflow_error("port wouldn't fit in a 16-bit value");

    workers.resize(n_workers);
    running.store(true);
    for (uint16_t i = 0; i < n_workers; i++)
        if (pthread_create(&workers[i], NULL, Epoll_Proxy, this) < 0)
            throw runtime_error("Could not create epoll thread");
}

void ServerImpl::Stop() {
    running.store(false);
}

void ServerImpl::Join() {
    void *retval;
    for (pthread_t &epoll_thread : workers) {
        if (pthread_join(epoll_thread, &retval)) throw runtime_error("pthread_join failed");
        if (retval) throw runtime_error("epoll thread had encountered an error");
    }
    workers.clear();
}

struct client_fd : ep_fd {
    int epoll_fd;
    size_t offset;
    bool flag_out;
    shared_ptr<Afina::Storage> ps;
    vector<char> buf;
    string out;
    std::list<client_fd> &list;
    std::list<client_fd>::iterator self;
    Protocol::Parser parser;
    client_fd(int fd_, int epoll_fd_, std::shared_ptr<Afina::Storage> ps_, std::list<client_fd> &list_, std::list<client_fd>::iterator self_)
            : ep_fd(fd_), epoll_fd(epoll_fd_), ps(ps_), offset(0), list(list_), self(self_), flag_out(false) {
        buf.resize(buffer_size);
    }
    
    void cleanup() {
        epoll_modify(epoll_fd, EPOLL_CTL_DEL, 0, *this);
        shutdown(fd, SHUT_RDWR);
        close(fd);
        list.erase(self);
    }
    
    void advance(uint32_t events) {
        if (events & (EPOLLHUP | EPOLLERR)) return cleanup();
        ssize_t len = 0; 
        do {
            offset += len;
            if (buf.size() == offset) buf.resize(std::max(buf.size() * 2, buffer_size));
            len = recv(fd, buf.data() + offset, buf.size() - offset, 0);
        } while (len > 0);

        if (errno != EWOULDBLOCK && errno != EAGAIN) return cleanup();
        if (!len) flag_out = true;
        if (!out.size()) {
            try {
                while (true) {
                    uint32_t body_size;
                    auto cmd = parser.Build(body_size);
                    if (!cmd) { 
                        size_t parsed = 0;
                        parser.Parse(buf.data(), offset, parsed);
                        buf.erase(buf.begin(), buf.begin() + parsed);
                        offset -= parsed;
                        cmd = parser.Build(body_size);
                    }
                    if (cmd && (!body_size || offset >= body_size + 2)) {
                        std::string body;
                        parser.Reset();
                        if (body_size) {
                            body.assign(buf.data(), body_size);
                            buf.erase(buf.begin(), buf.begin() + body_size + 2);
                            offset -= body_size + 2;
                        }
                        string local_out;
                        cmd->Execute(*ps, body, local_out);
                        out += local_out + string("\r\n");
                    } else break;
                }
            } catch (runtime_error &e) {
                out += string("CLIENT_ERROR ") + e.what() + string("\r\n");
                flag_out = true;
            }
        }
        if (out.size()) {
            while (out.size() && len > 0); {
                len = send(fd, out.data(), out.size(), 0);
                if (len > 0) out.erase(0, len);
            }
            if (errno != EWOULDBLOCK && errno != EAGAIN) return cleanup();
        }
        if (flag_out) return cleanup();
    }
};

struct listen_fd : ep_fd {
    int epoll_fd;
    std::shared_ptr<Afina::Storage> ps;
    std::list<client_fd> &client_list;
    listen_fd(int fd_, int epoll_fd_, std::shared_ptr<Afina::Storage> ps_, std::list<client_fd> &client_list_)
            : ep_fd(fd_), epoll_fd(epoll_fd_), ps(ps_), client_list(client_list_) {}

    void advance(uint32_t events)  {
        if (events & (EPOLLHUP | EPOLLERR)) {
            close(fd);
            throw runtime_error("Caught error state on listen socket");
        }
        sockaddr_in client_addr;
        socklen_t sinSize = sizeof(sockaddr_in);
        int client_socket;
        while ((client_socket = accept(fd, (struct sockaddr *)&client_addr, &sinSize)) != -1) { 
            if (setsocknonblocking(client_socket)) throw runtime_error("Couldn't set client socket to non-blocking");
            auto cl_it = client_list.emplace(client_list.end(), client_socket, epoll_fd, ps, client_list, client_list.end());
            cl_it->self = cl_it;
            if (epoll_modify(epoll_fd, EPOLL_CTL_ADD, EPOLLIN | EPOLLOUT | EPOLLET, *cl_it))
                throw runtime_error("epollctl failed to add client socket");
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return; 
        close(fd);
        throw runtime_error("Socket accept() failed");
    }
};

void ServerImpl::RunEpoll() {
    int epoll_sock = epoll_create1(0);
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;         // IPv4
    server_addr.sin_port = htons(listen_port);// TCP port number
    server_addr.sin_addr.s_addr = INADDR_ANY; // Bind to any address

    int server_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == -1) throw std::runtime_error("Failed to open socket");
    int opts = 1;
    if (setsockopt(server_socket, SOL_SOCKET, 0, &opts, sizeof(int)) == -1) {
        close(server_socket);
        throw std::runtime_error("Socket setsockopt() failed");
    }
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        close(server_socket);
        throw std::runtime_error("Socket bind() failed");
    }
    if (listen(server_socket, 5) == -1) {
            close(server_socket);
            throw runtime_error("Socket listen() failed");
    }
    std::list<client_fd> client_list; 
    listen_fd listening_object{server_socket, epoll_sock, pStorage, client_list}; 
    if (epoll_modify(epoll_sock, EPOLL_CTL_ADD, EPOLLIN | EPOLLET, listening_object))
        throw runtime_error("epoll_ctl failed to add the listen socket");

    while (running.load()) {
        epoll_event events[num_events];
        const int events_now = epoll_wait(epoll_sock, events, num_events, epoll_timeout);
        if (events_now < 0) {
            if (errno == EINTR) continue; 
            else throw runtime_error("networking epoll returned error");
        }
        for (auto i = 0; i < events_now; i++)
            static_cast<ep_fd*>(events[i].data.ptr)->advance(events[i].events);
    }
    shutdown(server_socket, SHUT_RDWR);
    close(server_socket);
    for (client_fd &cl : client_list) {
        shutdown(cl.fd, SHUT_RDWR);
        close(cl.fd);
    }
    close(epoll_sock);
}

} // namespace Blocking
} // namespace Network
} // namespace Afina
