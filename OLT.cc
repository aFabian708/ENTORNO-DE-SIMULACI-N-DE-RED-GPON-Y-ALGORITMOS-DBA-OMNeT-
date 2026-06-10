#include <omnetpp.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include "GponMsg_m.h"

using namespace omnetpp;
using namespace gpon_dba_lstm;

class OLT : public cSimpleModule
{
private:
    int totalOnus;
    double tickInterval;
    double assignableBytes;
    double demandScaleFactor;
    std::string dbaScheme;
    double margenConservadorBytes;

    std::vector<double> predictions;
    int predIndex;

    std::map<int, int64_t> reportedDemand;
    std::vector<std::map<int, int64_t>> demandHistory;
    int reactiveDelay;

    std::vector<double> onuShares;
    void loadShares(const char *filename);

    cOutVector utilizationVector;
    cOutVector wasteVector;
    cOutVector unmetVector;
    cOutVector grantVector;

    int64_t totalGranted;
    int64_t totalTransmitted;
    int64_t totalUnmet;
    int64_t totalWaste;
    int64_t tickCount;
    std::vector<int64_t> unmetHistory;
    int64_t totalQueueObserved;

    void loadPredictions(const char *filename);
    void runDBA();

protected:
    virtual void initialize() override;
    virtual void handleMessage(cMessage *msg) override;
    virtual void finish() override;
};

Define_Module(OLT);

void OLT::initialize()
{
    totalOnus = par("totalOnus");
    tickInterval = par("tickInterval");
    assignableBytes = par("assignableBytes");
    demandScaleFactor = par("demandScaleFactor");
    dbaScheme = par("dbaScheme").stringValue();
    margenConservadorBytes = par("margenConservadorBytes");
    reactiveDelay = par("reactiveDelay");

    totalGranted = 0;
    totalTransmitted = 0;
    totalUnmet = 0;
    totalWaste = 0;
    tickCount = 0;
    totalQueueObserved = 0;

    utilizationVector.setName("OLT_Utilization_pct");
    wasteVector.setName("OLT_Waste_bytes");
    unmetVector.setName("OLT_Unmet_bytes");
    grantVector.setName("OLT_Grant_bytes");

    if (dbaScheme == "lstm" || dbaScheme == "lstm_margen") {
        const char *predFile = par("predictionFile").stringValue();
        loadPredictions(predFile);
    }

    if (dbaScheme == "wrr") {
        const char *sharesFile = par("sharesFile").stringValue();
        loadShares(sharesFile);
    }

    for (int i = 0; i < totalOnus; ++i) {
        reportedDemand[i] = 0;
    }

    predIndex = 0;

    cMessage *tickMsg = new cMessage("DBA_Tick");
    scheduleAt(simTime() + tickInterval, tickMsg);
}

void OLT::handleMessage(cMessage *msg)
{
    if (msg->isSelfMessage()) {
        runDBA();
        tickCount++;
        scheduleAt(simTime() + tickInterval, msg);
    }
    else {
        DemandReport *report = dynamic_cast<DemandReport *>(msg);
        if (report) {
            int id = report->getOnuId();
            int64_t demand = report->getDemandBytes();
            reportedDemand[id] = demand;
        }
        delete msg;
    }
}

void OLT::loadPredictions(const char *filename)
{
    std::ifstream infile(filename);
    if (!infile.is_open()) {
        throw cRuntimeError("OLT: No se pudo abrir el archivo de predicciones: %s", filename);
    }
    std::string line;
    while (std::getline(infile, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        double val;
        if (ss >> val) {
            predictions.push_back(val);
        }
    }
    infile.close();
    EV << "OLT: Se cargaron " << predictions.size() << " predicciones desde " << filename << "\n";
}

void OLT::loadShares(const char *filename)
{
    std::ifstream infile(filename);
    if (!infile.is_open()) {
        throw cRuntimeError("OLT: No se pudo abrir el archivo de shares para WRR: %s", filename);
    }
    std::string line;
    double sum = 0.0;
    while (std::getline(infile, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        double val;
        if (ss >> val) {
            onuShares.push_back(val);
            sum += val;
        }
    }
    infile.close();

    if (onuShares.size() < (size_t)totalOnus) {
        throw cRuntimeError("OLT: El archivo de shares tiene menos elementos (%d) que totalOnus (%d)", 
                           (int)onuShares.size(), totalOnus);
    }

    if (sum > 0.0) {
        for (int i = 0; i < totalOnus; ++i) {
            onuShares[i] /= sum;
        }
    } else {
        for (int i = 0; i < totalOnus; ++i) {
            onuShares[i] = 1.0 / (double)totalOnus;
        }
    }
}

void OLT::runDBA()
{
    std::map<int, int64_t> currentDemand = reportedDemand;
    demandHistory.push_back(currentDemand);

    std::map<int, int64_t> allocatedGrants;
    for (int i = 0; i < totalOnus; ++i) {
        allocatedGrants[i] = 0;
    }

    int64_t sumaDemandasReales = 0;
    for (int i = 0; i < totalOnus; ++i) {
        sumaDemandasReales += currentDemand[i];
    }
    totalQueueObserved += sumaDemandasReales;

    if (dbaScheme == "reactive") {
        int targetIdx = (int)demandHistory.size() - 1 - reactiveDelay;
        std::map<int, int64_t> baseDemand;
        if (targetIdx >= 0) {
            baseDemand = demandHistory[targetIdx];
        } else {
            baseDemand = currentDemand;
        }

        int64_t totalBaseDemand = 0;
        for (int i = 0; i < totalOnus; ++i) {
            totalBaseDemand += baseDemand[i];
        }

        if (totalBaseDemand <= assignableBytes) {
            for (int i = 0; i < totalOnus; ++i) {
                allocatedGrants[i] = baseDemand[i];
            }
        } else {
            for (int i = 0; i < totalOnus; ++i) {
                double fraction = (totalBaseDemand > 0) ? (double)baseDemand[i] / (double)totalBaseDemand : 1.0 / totalOnus;
                allocatedGrants[i] = (int64_t)std::floor(fraction * assignableBytes);
            }
        }
    }
    else if (dbaScheme == "wrr") {
        for (int i = 0; i < totalOnus; ++i) {
            allocatedGrants[i] = (int64_t)std::floor(onuShares[i] * assignableBytes);
        }
    }
    else if (dbaScheme == "lstm" || dbaScheme == "lstm_margen") {
        double predGlobalNorm = 0.0;
        if (predIndex < (int)predictions.size()) {
            predGlobalNorm = predictions[predIndex];
            predIndex++;
        } else {
            predGlobalNorm = 0.0;
        }

        double predGlobalBytes = predGlobalNorm * demandScaleFactor;
        if (dbaScheme == "lstm_margen") {
            predGlobalBytes += margenConservadorBytes;
        }

        if (predGlobalBytes < 0.0) predGlobalBytes = 0.0;

        int64_t bolsaTotal = (int64_t)std::floor(std::min(predGlobalBytes, assignableBytes));

        int targetIdx = (int)demandHistory.size() - 1 - reactiveDelay;
        std::map<int, int64_t> baseDemand;
        if (targetIdx >= 0) {
            baseDemand = demandHistory[targetIdx];
        } else {
            baseDemand = currentDemand;
        }

        int64_t totalBaseDemand = 0;
        for (int i = 0; i < totalOnus; ++i) {
            totalBaseDemand += baseDemand[i];
        }

        if (totalBaseDemand <= bolsaTotal) {
            for (int i = 0; i < totalOnus; ++i) {
                allocatedGrants[i] = baseDemand[i];
            }
            int64_t residuo = bolsaTotal - totalBaseDemand;
            if (residuo > 0 && totalOnus > 0) {
                int64_t equitativo = residuo / totalOnus;
                for (int i = 0; i < totalOnus; ++i) {
                    allocatedGrants[i] += equitativo;
                }
            }
        } else {
            for (int i = 0; i < totalOnus; ++i) {
                double fraction = (totalBaseDemand > 0) ? (double)baseDemand[i] / (double)totalBaseDemand : 1.0 / totalOnus;
                allocatedGrants[i] = (int64_t)std::floor(fraction * bolsaTotal);
            }
        }
    }
    else {
        for (int i = 0; i < totalOnus; ++i) {
            allocatedGrants[i] = 0;
        }
    }

    int64_t tickGranted = 0;
    int64_t tickTransmitted = 0;
    int64_t tickWaste = 0;
    int64_t tickUnmet = 0;

    for (int i = 0; i < totalOnus; ++i) {
        int64_t g = allocatedGrants[i];
        int64_t d = currentDemand[i];

        tickGranted += g;

        if (g <= d) {
            tickTransmitted += g;
            tickUnmet += (d - g);
        } else {
            tickTransmitted += d;
            tickWaste += (g - d);
        }

        Grant *gMsg = new Grant("Grant");
        gMsg->setOnuId(i);
        gMsg->setGrantBytes(g);
        gMsg->setSimTime(simTime().dbl());
        send(gMsg, "out", i);
    }

    totalGranted += tickGranted;
    totalTransmitted += tickTransmitted;
    totalWaste += tickWaste;
    totalUnmet += tickUnmet;
    unmetHistory.push_back(tickUnmet);

    double uPct = (assignableBytes > 0) ? ((double)tickTransmitted / assignableBytes) * 100.0 : 0.0;
    if (uPct > 100.0) uPct = 100.0;

    utilizationVector.record(uPct);
    wasteVector.record(tickWaste);
    unmetVector.record(tickUnmet);
    grantVector.record(tickGranted);
}

void OLT::finish()
{
    double avgUtilization = 0.0;
    double avgWaste = 0.0;
    double avgUnmet = 0.0;
    double jitter = 0.0;

    if (tickCount > 0) {
        avgUtilization = (assignableBytes > 0) ? 
            ((double)totalTransmitted / ((double)tickCount * assignableBytes)) * 100.0 : 0.0;
        if (avgUtilization > 100.0) avgUtilization = 100.0;

        avgWaste = (double)totalWaste / (double)tickCount;
        avgUnmet = (double)totalUnmet / (double)tickCount;
    }

    if (unmetHistory.size() > 1) {
        double sumVar = 0.0;
        for (size_t t = 1; t < unmetHistory.size(); ++t) {
            double diff = (double)unmetHistory[t] - (double)unmetHistory[t-1];
            sumVar += (diff * diff);
        }
        jitter = sumVar / (double)(unmetHistory.size() - 1);
    }

    double tiempoEfectivo = (double)tickCount * tickInterval;
    double throughputMbps = (tiempoEfectivo > 0) ?
        ((double)totalTransmitted * 8.0) / (tiempoEfectivo * 1e6) : 0.0;

    double latenciaMs = 0.0;
    if (totalTransmitted > 0) {
        double colaMedia = (double)totalQueueObserved / (double)tickCount;
        double servicioMedio = (double)totalTransmitted / (double)tickCount;
        latenciaMs = (colaMedia / servicioMedio) * tickInterval * 1000.0;
    }

    recordScalar("avgUtilization_pct", avgUtilization);
    recordScalar("totalGranted_bytes", (double)totalGranted);
    recordScalar("totalTransmitted_bytes", (double)totalTransmitted);
    recordScalar("totalUnmet_bytes", (double)totalUnmet);
    recordScalar("totalWaste_bytes", (double)totalWaste);
    recordScalar("avgUnmet_bytes_per_tick", avgUnmet);
    recordScalar("avgWaste_bytes_per_tick", avgWaste);
    recordScalar("jitter_bytes_per_tick", jitter);
    recordScalar("throughput_mbps", throughputMbps);
    recordScalar("latency_ms", latenciaMs);
    recordScalar("tickCount", (double)tickCount);

    EV << "OLT finish - esquema: " << dbaScheme
       << ", Util media: " << avgUtilization << "%"
       << ", Throughput: " << throughputMbps << " Mbps"
       << ", Latencia aprox: " << latenciaMs << " ms\n";
}