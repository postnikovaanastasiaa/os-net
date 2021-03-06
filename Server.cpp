#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <errno.h>
#include <stdio.h>

#include <netinet/in.h> // sockaddr_in -- Internet Socket Address
#include <cstring>
#include <unistd.h>

#include <sstream>
#include <iostream>

#include "Server.h"


Server::Server(uint16_t port) : port(port),
        sfd(socket(AF_INET, SOCK_STREAM, 0 /* or IPPROTO_TCP*/)) {
    if (sfd == -1) {
        throw ServerException("Socket creation failed");
    }
    struct sockaddr_in socket_addr;

    memset(&socket_addr, 0, sizeof(struct sockaddr_in));
    socket_addr.sin_family = AF_INET;
    socket_addr.sin_addr.s_addr = INADDR_ANY;
    socket_addr.sin_port = htons(port);

    if (bind(sfd, (struct sockaddr*) &socket_addr, sizeof(sockaddr_in)) == -1) {
        closeFileDescriptor(sfd);
        throw ServerException("Bind failed");
    }

    if (listen(sfd, LISTEN_BACKLOG) == -1) {
        closeFileDescriptor(sfd);
        throw ServerException("Listen failed");
    }

    requestType.insert({"QUIT", Token::QUIT});
    requestType.insert({"GROUP", Token::GROUP});
    requestType.insert({"LISTGROUP", Token::LISTGROUP});
    requestType.insert({"POST", Token::POST});
    requestType.insert({"ARTICLE", Token::ARTICLE});
}

Server::~Server() {
    close(sfd);
}

void Server::run() {
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_size;
        int cfd = accept(sfd, (struct sockaddr *) &client_addr, &client_addr_size);
        if (cfd == -1) {
            continue;
        }

        clientDone = false;
        sendGreeting(cfd);

        while (!clientDone) {
            if (!readSingleLineRequest(cfd)) {
                break;
            }
            if (!requestType.count(requestTypeName)) {
                unsupportedOperation(cfd);
                continue;
            }
            Token token = requestType.find(requestTypeName)->second;
            switch (token) {
                case Token::QUIT:
                    quit(cfd);
                    break;
                case Token::POST:
                    post(cfd);
                    break;
                case Token::GROUP:
                    group(cfd);
                    break;
                case Token::LISTGROUP:
                    listGroup(cfd);
                    break;
                case Token::ARTICLE:
                    article(cfd);
                    break;
                default:
                    unsupportedOperation(cfd);
            }
        }
        closeFileDescriptor(cfd);
    }
}



void Server::closeFileDescriptor(int fd) {
    if (close(fd) == -1) {
        perror("File descriptor was not closed");
    }
}

void Server::sendMessage(std::string const &msg, int fd) {
    std::cout << "[S] " << msg << std::endl;

    int sent = 0;
    int curSent = 0;
    while (sent < msg.size()) {
        if ((curSent = send(fd, msg.substr(sent).data(), msg.size() - sent, 0)) == -1) {
            perror("Client didn't get the correct message :'(");
            clientDone = true;
            break;
        }
        sent += curSent;
    }
}

void Server::sendGreeting(int fd) {
    sendMessage("200\tNNTP Service Ready, posting permitted\r\n", fd);

}

bool Server::readSingleLineRequest(int cfd) {
    ssize_t received = 0;
    ssize_t read = 0;
    std::string request;
    while (received < 2 ||
        request[received - 2] != '\r' || request[received - 1] != '\n') {
        if ((read = recv(cfd, textBuffer, BUFFER_SIZE, 0)) == -1) {
            perror("Reading failed");
            clientDone = true;
            return false;
        } else if (read == 0) {
            // maybe eof?
            clientDone = true;
            return false;
        }
        received += read;
        request.append(std::string(textBuffer, read));
    }
    request = request.substr(0, request.length() - 2);
    std::cout << "[C] " << request << std::endl;
    processSingleLineRequest(request); // request must ends with \r\n
    return true;
}

void Server::processSingleLineRequest(std::string const &request) {
    std::istringstream stream(request);
    stream >> requestTypeName;
    requestArgument.clear();
    if (stream) {
        stream >> requestArgument;
    }
}


void Server::quit(int cfd) {
    sendMessage("205\tClosing connection\r\n", cfd);
    clientDone = true;
}

void Server::group(int fd) {
    if (!groups.count(requestArgument)) {
        sendMessage("411\tNo such newsgroup\r\n", fd);
        selectedGroup.clear();
        return;
    }
    selectedGroup = requestArgument;
    currentArticle = 1;
    sendMessage(getCurrentGroupRepresentation(), fd);
}

void Server::listGroup(int fd) {
    if (!requestArgument.empty()) {
        selectedGroup = requestArgument;
    } else if (selectedGroup.empty()) {
        sendMessage("412\tNo newsgroup selected\r\n", fd);
        return;
    }
     if (!groups.count(requestArgument)) {
        sendMessage("411\tNo such newsgroup\r\n", fd);
        selectedGroup.clear();
        return;
    }
     sendMessage(getCurrentGroupRepresentation() + getCurrentGroupArticlesId(), fd);
}

std::string Server::getCurrentGroupRepresentation() {
    auto& articlesID = groups.find(selectedGroup)->second;
    std::stringstream answer;

    answer << "221 " << articlesID.size() << " ";
    answer << articlesID.front() << " ";
    answer << articlesID.back() << " ";
    answer << requestArgument << "\r\n";
    return answer.str();
}

std::string Server::getCurrentGroupArticlesId() {
    auto& articlesID = groups.find(selectedGroup)->second;
    std::stringstream answer;

    for (auto id : articlesID) {
        answer << id << "\r\n";
    }
    answer << ".\r\n";
    return answer.str();
}

void Server::unsupportedOperation(int fd) {
    std::cout << requestTypeName << std::endl;
    sendMessage("500\tUnknown command\r\n", fd);
}

void Server::post(int cfd) {
    sendMessage("340\tInput article; end with <CR-LF>.<CR-LF>\r\n", cfd);

    std::string articleText;
    while (true) {
        ssize_t read = recv(cfd, articleBuffer, ARTICLE_SIZE, 0);
        if (read == -1) {
            sendMessage("441\tPosting failed\r\n", cfd);
            return;
        }
        articleText.append(std::string(textBuffer, read));
        const size_t id = articles.size() + 1;
        size_t groupsPos = articleText.find("Newsgroups:");
        if (groupsPos != std::string::npos) {
            size_t endGroupsPos = articleText.find("\r\n", groupsPos);
            std::stringstream groupsList(articleText.substr(groupsPos + 11, endGroupsPos - groupsPos - 11));
            std::string group;
            while (groupsList >> group) {
                auto it = groups.find(group);
                if (it == groups.end()) {
                    groups.insert({group, {id}});
                } else {
                    it->second.push_back(id);
                }
                std::cout << "Article " << id << " added to group " << group << std::endl;
            }
        }
        if (articleText.find("\r\n.\r\n")) {
            break;
        }
    }
    articles.push_back(articleText);
    sendMessage("240\tArticle received OK\r\n", cfd);
}

void Server::article(int fd) {
    size_t id = 0;
    if (requestArgument.empty()) {
        if (!selectedGroup.empty()) {
            id = groups.find(selectedGroup)->second[currentArticle - 1];
        } else {
            sendMessage("412\tNo newsgroup selected\r\n", fd);
            return;
        }
    } else {
        id = atoi(requestArgument.data());
        if (id == 0 || id > articles.size()) {
            sendMessage("430\tNo article with that message-id\r\n", fd);
            return;
        }
    }
    std::stringstream ans;
    ans << "202\t" << id << "\r\n";
    ans << articles[id - 1];
    sendMessage(ans.str(), fd);
}