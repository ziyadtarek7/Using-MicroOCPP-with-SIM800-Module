// matth-x/MicroOcpp
// Copyright Matthias Akstaller 2019 - 2023
// MIT License

#include <MicroOcpp/Model/ConnectorBase/Connector.h>

#include <MicroOcpp/Core/Context.h>
#include <MicroOcpp/Model/Model.h>
#include <MicroOcpp/Core/Request.h>
#include <MicroOcpp/Model/Transactions/TransactionStore.h>
#include <MicroOcpp/Core/Configuration.h>

#include <MicroOcpp/Operations/Authorize.h>
#include <MicroOcpp/Operations/StatusNotification.h>
#include <MicroOcpp/Operations/StartTransaction.h>
#include <MicroOcpp/Operations/StopTransaction.h>
#include <MicroOcpp/Operations/CiStrings.h>

#include <MicroOcpp/Debug.h>

#include <MicroOcpp/Model/Metering/MeteringService.h>
#include <MicroOcpp/Model/Reservation/ReservationService.h>
#include <MicroOcpp/Model/Authorization/AuthorizationService.h>

#include <MicroOcpp/Core/SimpleRequestFactory.h>

#ifndef MO_TX_CLEAN_ABORTED
#define MO_TX_CLEAN_ABORTED 1
#endif

using namespace MicroOcpp;
using namespace MicroOcpp::Ocpp16;

Connector::Connector(Context& context, int connectorId)
        : context(context), model(context.getModel()), connectorId{connectorId} {

    snprintf(availabilityBoolKey, sizeof(availabilityBoolKey), MO_CONFIG_EXT_PREFIX "AVAIL_CONN_%d", connectorId);
    availabilityBool = declareConfiguration<bool>(availabilityBoolKey, true, MO_KEYVALUE_FN, false, false, false);
    
    connectionTimeOutInt = declareConfiguration<int>("ConnectionTimeOut", 30);
    minimumStatusDurationInt = declareConfiguration<int>("MinimumStatusDuration", 0);
    stopTransactionOnInvalidIdBool = declareConfiguration<bool>("StopTransactionOnInvalidId", true);
    stopTransactionOnEVSideDisconnectBool = declareConfiguration<bool>("StopTransactionOnEVSideDisconnect", true);
    declareConfiguration<bool>("UnlockConnectorOnEVSideDisconnect", true, CONFIGURATION_VOLATILE, true);
    localPreAuthorizeBool = declareConfiguration<bool>("LocalPreAuthorize", false);
    localAuthorizeOfflineBool = declareConfiguration<bool>("LocalAuthorizeOffline", true);
    allowOfflineTxForUnknownIdBool = MicroOcpp::declareConfiguration<bool>("AllowOfflineTxForUnknownId", false);

    //if the EVSE goes offline, can it continue to charge without sending StartTx / StopTx to the server when going online again?
    silentOfflineTransactionsBool = declareConfiguration<bool>(MO_CONFIG_EXT_PREFIX "SilentOfflineTransactions", false);

    //how long the EVSE tries the Authorize request before it enters offline mode
    authorizationTimeoutInt = MicroOcpp::declareConfiguration<int>(MO_CONFIG_EXT_PREFIX "AuthorizationTimeout", 20);

    //FreeVend mode
    freeVendActiveBool = declareConfiguration<bool>(MO_CONFIG_EXT_PREFIX "FreeVendActive", false);
    freeVendIdTagString = declareConfiguration<const char*>(MO_CONFIG_EXT_PREFIX "FreeVendIdTag", "");

    if (!availabilityBool) {
        MO_DBG_ERR("Cannot declare availabilityBool");
    }

    if (model.getTransactionStore()) {
        transaction = model.getTransactionStore()->getLatestTransaction(connectorId);
    } else {
        MO_DBG_ERR("must initialize TxStore before Connector");
        (void)0;
    }
}

Connector::~Connector() {
    if (availabilityBool->getKey() == availabilityBoolKey) {
        availabilityBool->setKey(nullptr);
    }
}

ChargePointStatus Connector::getStatus() {
    /*
    * Handle special case: This is the Connector for the whole CP (i.e. connectorId=0) --> only states Available, Unavailable, Faulted are possible
    */
    if (connectorId == 0) {
        if (isFaulted()) {
            return ChargePointStatus::Faulted;
        } else if (!isOperative()) {
            return ChargePointStatus::Unavailable;
        } else {
            return ChargePointStatus::Available;
        }
    }

    if (isFaulted()) {
        return ChargePointStatus::Faulted;
    } else if (!isOperative()) {
        return ChargePointStatus::Unavailable;
    } else if (transaction && transaction->isRunning()) {
        //Transaction is currently running
        if (connectorPluggedInput && !connectorPluggedInput()) { //special case when StopTransactionOnEVSideDisconnect is false
            return ChargePointStatus::SuspendedEV;
        }
        if (!ocppPermitsCharge() ||
                (evseReadyInput && !evseReadyInput())) { 
            return ChargePointStatus::SuspendedEVSE;
        }
        if (evReadyInput && !evReadyInput()) {
            return ChargePointStatus::SuspendedEV;
        }
        return ChargePointStatus::Charging;
    } else if (model.getReservationService() && model.getReservationService()->getReservation(connectorId)) {
        return ChargePointStatus::Reserved;
    } else if ((!transaction || !transaction->isActive()) &&                 //no transaction preparation
               (!connectorPluggedInput || !connectorPluggedInput()) &&   //no vehicle plugged
               (!occupiedInput || !occupiedInput())) {                       //occupied override clear
        return ChargePointStatus::Available;
    } else {
        /*
         * Either in Preparing or Finishing state. Only way to know is from previous state
         */
        const auto previous = currentStatus;
        if (previous == ChargePointStatus::Finishing ||
                previous == ChargePointStatus::Charging ||
                previous == ChargePointStatus::SuspendedEV ||
                previous == ChargePointStatus::SuspendedEVSE) {
            return ChargePointStatus::Finishing;
        } else {
            return ChargePointStatus::Preparing;
        }
    }

    MO_DBG_DEBUG("status undefined");
    return ChargePointStatus::Faulted; //internal error
}

bool Connector::ocppPermitsCharge() {
    if (connectorId == 0) {
        MO_DBG_WARN("not supported for connectorId == 0");
        return false;
    }

    bool suspendDeAuthorizedIdTag = transaction && transaction->isIdTagDeauthorized(); //if idTag status is "DeAuthorized" and if charging should stop
    
    //check special case for DeAuthorized idTags: FreeVend mode
    if (suspendDeAuthorizedIdTag && freeVendActiveBool && freeVendActiveBool->getBool()) {
        suspendDeAuthorizedIdTag = false;
    }

    return transaction &&
            transaction->isRunning() &&
            transaction->isActive() &&
            !isFaulted() &&
            !suspendDeAuthorizedIdTag;
}

void Connector::loop() {

    if (!trackLoopExecute) {
        trackLoopExecute = true;
    }

    if (transaction && transaction->isAborted() && MO_TX_CLEAN_ABORTED) {
        //If the transaction is aborted (invalidated before started), delete all artifacts from flash
        //This is an optimization. The memory management will attempt to remove those files again later
        bool removed = true;
        if (auto mService = model.getMeteringService()) {
            removed &= mService->removeTxMeterData(connectorId, transaction->getTxNr());
        }

        if (removed) {
            removed &= model.getTransactionStore()->remove(connectorId, transaction->getTxNr());
        }

        if (removed) {
            model.getTransactionStore()->setTxEnd(connectorId, transaction->getTxNr()); //roll back creation of last tx entry
        }

        MO_DBG_DEBUG("collect aborted transaction %u-%u %s", connectorId, transaction->getTxNr(), removed ? "" : "failure");
        transaction = nullptr;
    }

    if (transaction && transaction->isAborted()) {
        MO_DBG_DEBUG("collect aborted transaction %u-%u", connectorId, transaction->getTxNr());
        transaction = nullptr;
    }

    if (transaction && transaction->isCompleted()) {
        MO_DBG_DEBUG("collect obsolete transaction %u-%u", connectorId, transaction->getTxNr());
        transaction = nullptr;
    }

    if (transaction) { //begin exclusively transaction-related operations
            
        if (connectorPluggedInput) {
            if (transaction->isRunning() && transaction->isActive() && !connectorPluggedInput()) {
                if (!stopTransactionOnEVSideDisconnectBool || stopTransactionOnEVSideDisconnectBool->getBool()) {
                    MO_DBG_DEBUG("Stop Tx due to EV disconnect");
                    transaction->setStopReason("EVDisconnected");
                    transaction->setInactive();
                    transaction->commit();
                }
            }

            if (transaction->isActive() &&
                    !transaction->getStartSync().isRequested() &&
                    transaction->getBeginTimestamp() > MIN_TIME &&
                    connectionTimeOutInt && connectionTimeOutInt->getInt() > 0 &&
                    !connectorPluggedInput() &&
                    model.getClock().now() - transaction->getBeginTimestamp() >= connectionTimeOutInt->getInt()) {

                MO_DBG_INFO("Session mngt: timeout");
                transaction->setInactive();
                transaction->commit();

                updateTxNotification(TxNotification::ConnectionTimeout);
            }
        }

        if (transaction->isActive() &&
                transaction->isIdTagDeauthorized() && ( //transaction has been deAuthorized
                    !transaction->isRunning() ||        //if transaction hasn't started yet, always end
                    !stopTransactionOnInvalidIdBool || stopTransactionOnInvalidIdBool->getBool())) { //if transaction is running, behavior depends on StopTransactionOnInvalidId
            
            MO_DBG_DEBUG("DeAuthorize session");
            transaction->setStopReason("DeAuthorized");
            transaction->setInactive();
            transaction->commit();
        }

        /*
        * Check conditions for start or stop transaction
        */

        if (!transaction->isRunning()) {
            //start tx?

            if (transaction->isActive() && transaction->isAuthorized() &&  //tx must be authorized
                    (!connectorPluggedInput || connectorPluggedInput()) && //if applicable, connector must be plugged
                    isOperative() && //only start tx if charger is free of error conditions
                    (!startTxReadyInput || startTxReadyInput())) { //if defined, user Input for allowing StartTx must be true
                //start Transaction

                MO_DBG_INFO("Session mngt: trigger StartTransaction");

                auto meteringService = model.getMeteringService();
                if (transaction->getMeterStart() < 0 && meteringService) {
                    auto meterStart = meteringService->readTxEnergyMeter(transaction->getConnectorId(), ReadingContext::TransactionBegin);
                    if (meterStart && *meterStart) {
                        transaction->setMeterStart(meterStart->toInteger());
                    } else {
                        MO_DBG_ERR("MeterStart undefined");
                    }
                }

                if (transaction->getStartTimestamp() <= MIN_TIME) {
                    transaction->setStartTimestamp(model.getClock().now());
                    transaction->setStartBootNr(model.getBootNr());
                }

                updateTxNotification(TxNotification::StartTx);

                if (transaction->isSilent()) {
                    MO_DBG_INFO("silent Transaction: omit StartTx");
                    transaction->getStartSync().setRequested();
                    transaction->getStartSync().confirm();
                    transaction->commit();
                    return;
                }

                transaction->commit();

                if (model.getMeteringService()) {
                    model.getMeteringService()->beginTxMeterData(transaction.get());
                }

                auto startTx = makeRequest(new StartTransaction(model, transaction));
                startTx->setTimeout(0);
                startTx->setOnReceiveConfListener([this] (JsonObject response) {
                    //fetch authorization status from StartTransaction.conf() for user notification

                    const char* idTagInfoStatus = response["idTagInfo"]["status"] | "_Undefined";
                    if (strcmp(idTagInfoStatus, "Accepted")) {
                        updateTxNotification(TxNotification::DeAuthorized);
                    }
                });
                context.initiateRequest(std::move(startTx));
                return;
            }
        } else  {
            //stop tx?

            if (!transaction->isActive() &&
                    (!stopTxReadyInput || stopTxReadyInput())) {
                //stop transaction

                MO_DBG_INFO("Session mngt: trigger StopTransaction");

                if (transaction->isSilent()) {
                    MO_DBG_INFO("silent Transaction: omit StopTx");
                    updateTxNotification(TxNotification::StopTx);
                    transaction->getStopSync().setRequested();
                    transaction->getStopSync().confirm();
                    if (auto mService = model.getMeteringService()) {
                        mService->removeTxMeterData(connectorId, transaction->getTxNr());
                    }
                    model.getTransactionStore()->remove(connectorId, transaction->getTxNr());
                    model.getTransactionStore()->setTxEnd(connectorId, transaction->getTxNr());
                    transaction = nullptr;
                    return;
                }
                
                auto meteringService = model.getMeteringService();
                if (transaction->getMeterStop() < 0 && meteringService) {
                    auto meterStop = meteringService->readTxEnergyMeter(transaction->getConnectorId(), ReadingContext::TransactionEnd);
                    if (meterStop && *meterStop) {
                        transaction->setMeterStop(meterStop->toInteger());
                    } else {
                        MO_DBG_ERR("MeterStop undefined");
                    }
                }

                if (transaction->getStopTimestamp() <= MIN_TIME) {
                    transaction->setStopTimestamp(model.getClock().now());
                    transaction->setStopBootNr(model.getBootNr());
                }

                transaction->commit();

                updateTxNotification(TxNotification::StopTx);

                std::shared_ptr<TransactionMeterData> stopTxData;

                if (meteringService) {
                    stopTxData = meteringService->endTxMeterData(transaction.get());
                }

                std::unique_ptr<Request> stopTx;

                if (stopTxData) {
                    stopTx = makeRequest(new StopTransaction(model, std::move(transaction), stopTxData->retrieveStopTxData()));
                } else {
                    stopTx = makeRequest(new StopTransaction(model, std::move(transaction)));
                }
                stopTx->setTimeout(0);
                context.initiateRequest(std::move(stopTx));
                return;
            }
        }
    } //end transaction-related operations

    //handle FreeVend mode
    if (freeVendActiveBool && freeVendActiveBool->getBool() && connectorPluggedInput) {
        if (!freeVendTrackPlugged && connectorPluggedInput() && !transaction) {
            const char *idTag = freeVendIdTagString ? freeVendIdTagString->getString() : "";
            if (!idTag || *idTag == '\0') {
                idTag = "A0000000";
            }
            MO_DBG_INFO("begin FreeVend Tx using idTag %s", idTag);
            beginTransaction_authorized(idTag);
            
            if (!transaction) {
                MO_DBG_ERR("could not begin FreeVend Tx");
                (void)0;
            }
        }

        freeVendTrackPlugged = connectorPluggedInput();
    }

    auto status = getStatus();

    for (auto i = std::min(errorDataInputs.size(), trackErrorDataInputs.size()); i >= 1; i--) {
        auto index = i - 1;
        auto error = errorDataInputs[index].operator()();
        if (error.isError && !trackErrorDataInputs[index]) {
            //new error
            auto statusNotification = makeRequest(
                    new StatusNotification(connectorId, status, model.getClock().now(), error));
            statusNotification->setTimeout(0);
            context.initiateRequest(std::move(statusNotification));

            currentStatus = status;
            reportedStatus = status;
            trackErrorDataInputs[index] = true;
        } else if (!error.isError && trackErrorDataInputs[index]) {
            //reset error
            trackErrorDataInputs[index] = false;
        }
    }
    
    if (status != currentStatus) {
        currentStatus = status;
        t_statusTransition = mocpp_tick_ms();
        MO_DBG_DEBUG("Status changed%s", minimumStatusDurationInt->getInt() ? ", will report delayed" : "");
    }

    if (reportedStatus != currentStatus &&
            model.getClock().now() >= Timestamp(2010,0,0,0,0,0) &&
            (minimumStatusDurationInt->getInt() <= 0 || //MinimumStatusDuration disabled
            mocpp_tick_ms() - t_statusTransition >= ((unsigned long) minimumStatusDurationInt->getInt()) * 1000UL)) {
        reportedStatus = currentStatus;
        Timestamp reportedTimestamp = model.getClock().now();
        reportedTimestamp -= (mocpp_tick_ms() - t_statusTransition) / 1000UL;

        auto statusNotification = makeRequest(
                new StatusNotification(connectorId, reportedStatus, reportedTimestamp, getErrorCode()));
        statusNotification->setTimeout(0);
        context.initiateRequest(std::move(statusNotification));
        return;
    }

    return;
}

bool Connector::isFaulted() {
    //for (auto i = errorDataInputs.begin(); i != errorDataInputs.end(); ++i) {
    for (size_t i = 0; i < errorDataInputs.size(); i++) {
        if (errorDataInputs[i].operator()().isFaulted) {
            return true;
        }
    }
    return false;
}

const char *Connector::getErrorCode() {
    for (auto i = errorDataInputs.size(); i >= 1; i--) {
        auto error = errorDataInputs[i-1].operator()();
        if (error.isError && error.errorCode) {
            return error.errorCode;
        }
    }
    return nullptr;
}

std::shared_ptr<Transaction> Connector::allocateTransaction() {

    decltype(allocateTransaction()) tx;

    //clean possible aorted tx
    auto txr = model.getTransactionStore()->getTxEnd(connectorId);
    auto txsize = model.getTransactionStore()->size(connectorId);
    for (decltype(txsize) i = 0; i < txsize; i++) {
        txr = (txr + MAX_TX_CNT - 1) % MAX_TX_CNT; //decrement by 1
        
        auto tx = model.getTransactionStore()->getTransaction(connectorId, txr);
        //check if dangling silent tx, aborted tx, or corrupted entry (tx == null)
        if (!tx || tx->isSilent() || (tx->isAborted() && MO_TX_CLEAN_ABORTED)) {
            //yes, remove
            bool removed = true;
            if (auto mService = model.getMeteringService()) {
                removed &= mService->removeTxMeterData(connectorId, txr);
            }
            if (removed) {
                removed &= model.getTransactionStore()->remove(connectorId, txr);
            }
            if (removed) {
                model.getTransactionStore()->setTxEnd(connectorId, txr);
                MO_DBG_WARN("deleted dangling silent or aborted tx for new transaction");
            } else {
                MO_DBG_ERR("memory corruption");
                break;
            }
        } else {
            //no, tx record trimmed, end
            break;
        }
    }

    //try to create new transaction
    tx = model.getTransactionStore()->createTransaction(connectorId);

    if (!tx) {
        //could not create transaction - now, try to replace tx history entry

        auto txl = model.getTransactionStore()->getTxBegin(connectorId);
        auto txsize = model.getTransactionStore()->size(connectorId);

        for (decltype(txsize) i = 0; i < txsize; i++) {

            if (tx) {
                //success, finished here
                break;
            }

            //no transaction allocated, delete history entry to make space

            auto txhist = model.getTransactionStore()->getTransaction(connectorId, txl);
            //oldest entry, now check if it's history and can be removed or corrupted entry
            if (!txhist || txhist->isCompleted() || txhist->isAborted()) {
                //yes, remove
                bool removed = true;
                if (auto mService = model.getMeteringService()) {
                    removed &= mService->removeTxMeterData(connectorId, txl);
                }
                if (removed) {
                    removed &= model.getTransactionStore()->remove(connectorId, txl);
                }
                if (removed) {
                    model.getTransactionStore()->setTxBegin(connectorId, (txl + 1) % MAX_TX_CNT);
                    MO_DBG_DEBUG("deleted tx history entry for new transaction");

                    tx = model.getTransactionStore()->createTransaction(connectorId);
                } else {
                    MO_DBG_ERR("memory corruption");
                    break;
                }
            } else {
                //no, end of history reached, don't delete further tx
                MO_DBG_DEBUG("cannot delete more tx");
                break;
            }

            txl++;
            txl %= MAX_TX_CNT;
        }
    }

    if (!tx) {
        //couldn't create normal transaction -> check if to start charging without real transaction
        if (silentOfflineTransactionsBool && silentOfflineTransactionsBool->getBool()) {
            //try to handle charging session without sending StartTx or StopTx to the server
            tx = model.getTransactionStore()->createTransaction(connectorId, true);

            if (tx) {
                MO_DBG_DEBUG("created silent transaction");
                (void)0;
            }
        }
    }

    return tx;
}

std::shared_ptr<Transaction> Connector::beginTransaction(const char *idTag) {

    if (transaction) {
        MO_DBG_WARN("tx process still running. Please call endTransaction(...) before");
        return nullptr;
    }

    MO_DBG_DEBUG("Begin transaction process (%s), prepare", idTag != nullptr ? idTag : "");
    
    AuthorizationData *localAuth = nullptr;
    bool offlineBlockedAuth = false; //if offline authorization will be blocked by local auth list entry

    //check local OCPP whitelist
    if (model.getAuthorizationService()) {
        localAuth = model.getAuthorizationService()->getLocalAuthorization(idTag);

        //check authorization status
        if (localAuth && localAuth->getAuthorizationStatus() != AuthorizationStatus::Accepted) {
            MO_DBG_DEBUG("local auth denied (%s)", idTag);
            offlineBlockedAuth = true;
            localAuth = nullptr;
        }

        //check expiry
        if (localAuth && localAuth->getExpiryDate() && *localAuth->getExpiryDate() < model.getClock().now()) {
            MO_DBG_DEBUG("idTag %s local auth entry expired", idTag);
            offlineBlockedAuth = true;
            localAuth = nullptr;
        }
    }

    Reservation *reservation = nullptr;
    bool offlineBlockedResv = false; //if offline authorization will be blocked by reservation

    //check if blocked by reservation
    if (model.getReservationService()) {
        const char *parentIdTag = localAuth ? localAuth->getParentIdTag() : nullptr;

        reservation = model.getReservationService()->getReservation(
                connectorId,
                idTag,
                parentIdTag);

        if (reservation && !reservation->matches(
                    idTag,
                    parentIdTag)) {
            //reservation blocks connector
            offlineBlockedResv = true; //when offline, tx is always blocked

            //if parentIdTag is known, abort this tx immediately, otherwise wait for Authorize.conf to decide
            if (parentIdTag) {
                //parentIdTag known
                MO_DBG_INFO("connector %u reserved - abort transaction", connectorId);
                updateTxNotification(TxNotification::ReservationConflict);
                return nullptr;
            } else {
                //parentIdTag unkown but local authorization failed in any case
                MO_DBG_INFO("connector %u reserved - no local auth", connectorId);
                localAuth = nullptr;
            }
        }
    }
    
    transaction = allocateTransaction();

    if (!transaction) {
        MO_DBG_ERR("could not allocate Tx");
        return nullptr;
    }

    if (!idTag || *idTag == '\0') {
        //input string is empty
        transaction->setIdTag("");
    } else {
        transaction->setIdTag(idTag);
    }

    transaction->setBeginTimestamp(model.getClock().now());

    //check for local preauthorization
    if (localAuth && localPreAuthorizeBool && localPreAuthorizeBool->getBool()) {
        MO_DBG_DEBUG("Begin transaction process (%s), preauthorized locally", idTag != nullptr ? idTag : "");

        if (reservation) {
            transaction->setReservationId(reservation->getReservationId());
        }
        transaction->setAuthorized();

        updateTxNotification(TxNotification::Authorized);
    }

    transaction->commit();

    auto authorize = makeRequest(new Authorize(context.getModel(), idTag));
    authorize->setTimeout(authorizationTimeoutInt && authorizationTimeoutInt->getInt() > 0 ? authorizationTimeoutInt->getInt() * 1000UL : 20UL * 1000UL);
    auto tx = transaction;
    authorize->setOnReceiveConfListener([this, tx] (JsonObject response) {
        JsonObject idTagInfo = response["idTagInfo"];

        if (strcmp("Accepted", idTagInfo["status"] | "UNDEFINED")) {
            //Authorization rejected, abort transaction
            MO_DBG_DEBUG("Authorize rejected (%s), abort tx process", tx->getIdTag());
            tx->setIdTagDeauthorized();
            tx->commit();
            updateTxNotification(TxNotification::AuthorizationRejected);
            return;
        }

        if (model.getReservationService()) {
            auto reservation = model.getReservationService()->getReservation(
                        connectorId,
                        tx->getIdTag(),
                        idTagInfo["parentIdTag"] | (const char*) nullptr);
            if (reservation) {
                //reservation found for connector
                if (reservation->matches(
                            tx->getIdTag(),
                            idTagInfo["parentIdTag"] | (const char*) nullptr)) {
                    MO_DBG_INFO("connector %u matches reservationId %i", connectorId, reservation->getReservationId());
                    tx->setReservationId(reservation->getReservationId());
                } else {
                    //reservation found for connector but does not match idTag or parentIdTag
                    MO_DBG_INFO("connector %u reserved - abort transaction", connectorId);
                    tx->setInactive();
                    tx->commit();
                    updateTxNotification(TxNotification::ReservationConflict);
                    return;
                }
            }
        }

        MO_DBG_DEBUG("Authorized transaction process (%s)", tx->getIdTag());
        tx->setAuthorized();
        tx->commit();

        updateTxNotification(TxNotification::Authorized);
    });

    //capture local auth and reservation check in for timeout handler
    bool localAuthFound = localAuth;
    bool reservationFound = reservation;
    int reservationId = reservation ? reservation->getReservationId() : -1;
    authorize->setOnTimeoutListener([this, tx,
                offlineBlockedAuth, 
                offlineBlockedResv, 
                localAuthFound,
                reservationFound,
                reservationId] () {

        if (offlineBlockedAuth) {
            //local auth entry exists, but is expired -> avoid offline tx
            MO_DBG_DEBUG("Abort transaction process (%s), timeout, expired local auth", tx->getIdTag());
            tx->setInactive();
            tx->commit();
            updateTxNotification(TxNotification::AuthorizationTimeout);
            return;
        }

        if (offlineBlockedResv) {
            //reservation found for connector but does not match idTag or parentIdTag
            MO_DBG_INFO("connector %u reserved (offline) - abort transaction", connectorId);
            tx->setInactive();
            tx->commit();
            updateTxNotification(TxNotification::ReservationConflict);
            return;
        }

        if (localAuthFound && localAuthorizeOfflineBool && localAuthorizeOfflineBool->getBool()) {
            MO_DBG_DEBUG("Offline transaction process (%s), locally authorized", tx->getIdTag());
            if (reservationFound) {
                tx->setReservationId(reservationId);
            }
            tx->setAuthorized();
            tx->commit();

            updateTxNotification(TxNotification::Authorized);
            return;
        }

        if (allowOfflineTxForUnknownIdBool && allowOfflineTxForUnknownIdBool->getBool()) {
            MO_DBG_DEBUG("Offline transaction process (%s), allow unknown ID", tx->getIdTag());
            if (reservationFound) {
                tx->setReservationId(reservationId);
            }
            tx->setAuthorized();
            tx->commit();
            updateTxNotification(TxNotification::Authorized);
            return;
        }

        MO_DBG_DEBUG("Abort transaction process (%s): timeout", tx->getIdTag());
        tx->setInactive();
        tx->commit();
        updateTxNotification(TxNotification::AuthorizationTimeout);
        return; //offline tx disabled
    });
    context.initiateRequest(std::move(authorize));

    return transaction;
}

std::shared_ptr<Transaction> Connector::beginTransaction_authorized(const char *idTag, const char *parentIdTag) {
    
    if (transaction) {
        MO_DBG_WARN("tx process still running. Please call endTransaction(...) before");
        return nullptr;
    }

    transaction = allocateTransaction();

    if (!transaction) {
        MO_DBG_ERR("could not allocate Tx");
        return nullptr;
    }

    if (!idTag || *idTag == '\0') {
        //input string is empty
        transaction->setIdTag("");
    } else {
        transaction->setIdTag(idTag);
    }

    transaction->setBeginTimestamp(model.getClock().now());
    
    MO_DBG_DEBUG("Begin transaction process (%s), already authorized", idTag != nullptr ? idTag : "");

    transaction->setAuthorized();
    
    if (model.getReservationService()) {
        if (auto reservation = model.getReservationService()->getReservation(connectorId, idTag, parentIdTag)) {
            if (reservation->matches(idTag, parentIdTag)) {
                transaction->setReservationId(reservation->getReservationId());
            }
        }
    }

    transaction->commit();

    return transaction;
}

void Connector::endTransaction(const char *idTag, const char *reason) {

    if (!transaction || !transaction->isActive()) {
        //transaction already ended / not active anymore
        return;
    }

    MO_DBG_DEBUG("End session started by idTag %s",
                            transaction->getIdTag());
    
    if (idTag && *idTag != '\0') {
        transaction->setStopIdTag(idTag);
    }
    
    if (reason) {
        transaction->setStopReason(reason);
    }
    transaction->setInactive();
    transaction->commit();
}

std::shared_ptr<Transaction>& Connector::getTransaction() {
    return transaction;
}

bool Connector::isOperative() {
    if (isFaulted()) {
        return false;
    }

    if (!trackLoopExecute) {
        return false;
    }

    //check for running transaction(s) - if yes then the connector is always operative
    if (connectorId == 0) {
        for (unsigned int cId = 1; cId < model.getNumConnectors(); cId++) {
            if (model.getConnector(cId)->getTransaction() && model.getConnector(cId)->getTransaction()->isRunning()) {
                return true;
            }
        }
    } else {
        if (transaction && transaction->isRunning()) {
            return true;
        }
    }

    return availabilityVolatile && availabilityBool->getBool();
}

void Connector::setAvailability(bool available) {
    availabilityBool->setBool(available);
    configuration_save();
}

void Connector::setAvailabilityVolatile(bool available) {
    availabilityVolatile = available;
}

void Connector::setConnectorPluggedInput(std::function<bool()> connectorPlugged) {
    this->connectorPluggedInput = connectorPlugged;
}

void Connector::setEvReadyInput(std::function<bool()> evRequestsEnergy) {
    this->evReadyInput = evRequestsEnergy;
}

void Connector::setEvseReadyInput(std::function<bool()> connectorEnergized) {
    this->evseReadyInput = connectorEnergized;
}

void Connector::addErrorCodeInput(std::function<const char*()> connectorErrorCode) {
    addErrorDataInput([connectorErrorCode] () -> ErrorData {
        return ErrorData(connectorErrorCode());
    });
}

void Connector::addErrorDataInput(std::function<ErrorData ()> errorDataInput) {
    this->errorDataInputs.push_back(errorDataInput);
    this->trackErrorDataInputs.push_back(false);
}

void Connector::setOnUnlockConnector(std::function<PollResult<bool>()> unlockConnector) {
    this->onUnlockConnector = unlockConnector;
}

std::function<PollResult<bool>()> Connector::getOnUnlockConnector() {
    return this->onUnlockConnector;
}

void Connector::setStartTxReadyInput(std::function<bool()> startTxReady) {
    this->startTxReadyInput = startTxReady;
}

void Connector::setStopTxReadyInput(std::function<bool()> stopTxReady) {
    this->stopTxReadyInput = stopTxReady;
}

void Connector::setOccupiedInput(std::function<bool()> occupied) {
    this->occupiedInput = occupied;
}

void Connector::setTxNotificationOutput(std::function<void(Transaction*, TxNotification)> txNotificationOutput) {
    this->txNotificationOutput = txNotificationOutput;
}

void Connector::updateTxNotification(TxNotification event) {
    if (txNotificationOutput) {
        txNotificationOutput(transaction.get(), event);
    }
}
