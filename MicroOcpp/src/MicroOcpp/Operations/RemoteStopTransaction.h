// matth-x/MicroOcpp
// Copyright Matthias Akstaller 2019 - 2023
// MIT License

#ifndef REMOTESTOPTRANSACTION_H
#define REMOTESTOPTRANSACTION_H

#include <MicroOcpp/Core/Operation.h>

namespace MicroOcpp {

class Model;

namespace Ocpp16 {

class RemoteStopTransaction : public Operation {
private:
    Model& model;
    bool accepted = false;

    const char *errorCode = nullptr;
public:
    RemoteStopTransaction(Model& model);

    const char* getOperationType() override;

    void processReq(JsonObject payload) override;

    std::unique_ptr<DynamicJsonDocument> createConf() override;

    const char *getErrorCode() override {return errorCode;}
};

} //end namespace Ocpp16
} //end namespace MicroOcpp
#endif
