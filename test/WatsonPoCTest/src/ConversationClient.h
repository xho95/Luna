#pragma once

#include <string>
#include <memory>

#include "json/value.h"
#include "restclient-cpp/connection.h"

#include "EnvironmentInfo.h"

class ConversationClient
{
public:
    ConversationClient();
    ~ConversationClient();

public:
    int sendText(const std::string& message, std::string& output_text, std::string& intent, std::string& body);
    Json::Value& getContext() { return context_; }

private:
    bool getToken();
    bool createNewConversation();

private:
    EnvironmentInfo env_;

    std::string token_;
    std::string conversation_id_;
    Json::Value context_;

    std::shared_ptr<RestClient::Connection> rest_conn_;
};

