#include <omnetpp.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdint>
#include <cmath>
#include "GponMsg_m.h"

using namespace omnetpp;
using namespace gpon_dba_lstm;

class ONU : public cSimpleModule
{
private:
    int onuId;
    int totalOnus;
    double tickInterval;
    double demandScaleFactor;
    std::vector<double> demandSeries;
    int64_t queueBytes;
    int64_t bufferSize;
    int64_t droppedBytes;
    int64_t generatedBytes;
    cMessage *tickMsg;

    // Heterogeneidad coherente con el dataset
    double onuShare;   // fraccion normalizada de esta ONU (factor_i / suma_factores)

    cOutVector queueVector;

    void loadDemand(const char *filename);
    void loadShare(const char *filename);

public:
    virtual ~ONU(); // <-- AÑADIDO: Destructor requerido por OMNeT++

protected:
    virtual void initialize() override;
    virtual void handleMessage(cMessage *msg) override;
    virtual void finish() override;
};

Define_Module(ONU);

// Implementación del destructor para evitar fugas de memoria
ONU::~ONU()
{
    cancelAndDelete(tickMsg);
}

void ONU::initialize()
{
    onuId             = par("onuId");
    totalOnus         = par("totalOnus");
    tickInterval      = par("tickInterval").doubleValue();
    demandScaleFactor = par("demandScaleFactor").doubleValue();
    queueBytes     = 0;
    bufferSize     = 225000LL;
    droppedBytes   = 0;
    generatedBytes = 0;
    onuShare       = 1.0 / (double)totalOnus;  // default uniforme si no carga factores

    // === Heterogeneidad coherente con el dataset ===
    const char *factoresFile = par("factoresFile").stringValue();
    if (strlen(factoresFile) > 0)
        loadShare(factoresFile);

    EV << "ONU " << onuId << ": share = " << onuShare
       << " (heterogeneidad coherente con dataset)\n";

    const char *demandFile = par("demandFile").stringValue();
    if (strlen(demandFile) > 0)
        loadDemand(demandFile);

    queueVector.setName("queueBytes");

    tickMsg = new cMessage("tick");
    scheduleAt(simTime() + tickInterval, tickMsg);
}

void ONU::loadShare(const char *filename)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        EV_WARN << "ONU " << onuId << ": no se pudo abrir factores " << filename
                << " (usando reparto uniforme)\n";
        return;
    }

    std::vector<double> factores;
    std::string line;
    while (std::getline(file, line)) {
        // <-- AÑADIDO: protección contra el retorno de carro (\r) de Windows
        if (!line.empty() && line != "\r") 
            factores.push_back(std::stod(line));
    }

    if ((int)factores.size() < totalOnus) {
        EV_WARN << "ONU " << onuId << ": factores insuficientes ("
                << factores.size() << " < " << totalOnus
                << "), usando reparto uniforme\n";
        return;
    }

    double suma = 0.0;
    for (int i = 0; i < totalOnus; i++)
        suma += factores[i];

    if (suma > 0 && onuId < (int)factores.size())
        onuShare = factores[onuId] / suma;
}

void ONU::loadDemand(const char *filename)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        EV_WARN << "ONU " << onuId << ": no se pudo abrir " << filename << "\n";
        return;
    }

    std::string line;
    std::getline(file, line); // saltar cabecera

    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string t, d;
        
        // Efectivamente, esto solo tomará las dos primeras columnas y descartará el resto
        std::getline(ss, t, ',');
        std::getline(ss, d, ',');
        
        if (!d.empty() && d != "\r") // Protección extra
            demandSeries.push_back(std::stod(d));
    }
    EV << "ONU " << onuId << ": cargadas " << demandSeries.size() << " muestras\n";
}

void ONU::handleMessage(cMessage *msg)
{
    if (msg == tickMsg) {
        // === Muestreo sincronizado del CSV con reparto heterogeneo ===
        if (!demandSeries.empty()) {
            int64_t globalTick = (int64_t)std::round(simTime().dbl() / tickInterval);
            int64_t idx        = globalTick % (int64_t)demandSeries.size();
            double rawDemand   = demandSeries[idx] * onuShare;
            int64_t newBytes   = (int64_t)(rawDemand * demandScaleFactor);
            generatedBytes    += newBytes;

            // Drop-Tail: si la cola no tiene espacio, se descarta el exceso
            int64_t spaceAvailable = bufferSize - queueBytes;
            if (spaceAvailable <= 0) {
                droppedBytes += newBytes;
            } else if (newBytes > spaceAvailable) {
                droppedBytes += (newBytes - spaceAvailable);
                queueBytes    = bufferSize;
            } else {
                queueBytes   += newBytes;
            }
        }

        queueVector.record((double)queueBytes);

        DemandReport *report = new DemandReport("report");
        report->setOnuId(onuId);
        report->setDemandBytes((long)queueBytes);
        report->setSimTime(simTime().dbl());
        send(report, "out");

        scheduleAt(simTime() + tickInterval, tickMsg);

    } else {
        Grant *grant = check_and_cast<Grant *>(msg);
        int64_t grantBytes = (int64_t)grant->getGrantBytes();

        queueBytes = std::max((int64_t)0, queueBytes - grantBytes);
        queueVector.record((double)queueBytes);

        delete grant;
    }
}

void ONU::finish()
{
    recordScalar("finalQueueBytes",  (double)queueBytes);
    recordScalar("droppedBytes",     (double)droppedBytes);
    recordScalar("generatedBytes",   (double)generatedBytes);
    recordScalar("onuShare",         onuShare);
    double plr = (generatedBytes > 0) ?
                 (double)droppedBytes / (double)generatedBytes * 100.0 : 0.0;
    recordScalar("packetLossRatio_pct", plr);
}