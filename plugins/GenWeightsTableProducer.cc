#include "FWCore/Framework/interface/global/EDProducer.h"
#include "FWCore/Framework/interface/Event.h"
#include "FWCore/Framework/interface/Run.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "PhysicsTools/NanoAOD/interface/FlatTable.h"
#include "PhysicsTools/NanoAOD/interface/MergableCounterTable.h"
#include "SimDataFormats/GeneratorProducts/interface/GenEventInfoProduct.h"
#include "SimDataFormats/GeneratorProducts/interface/LHEEventProduct.h"
#include "SimDataFormats/GeneratorProducts/interface/LHERunInfoProduct.h"
#include "FWCore/MessageLogger/interface/MessageLogger.h"

#include <vector>
#include <iostream>
#include <regex>

namespace {
    ///  ---- Cache object for running sums of weights ----
    struct Counter {
        Counter() : 
            num(0), sumw(0), sumw2(0), sumPDF(), sumScale(), sumNamed() {}

        // the counters
        long long num;
        long double sumw;
        long double sumw2;
        std::vector<long double> sumPDF, sumScale, sumNamed;

        void clear() { 
            num = 0; sumw = 0; sumw2 = 0;
            sumPDF.clear(); sumScale.clear(); sumNamed.clear();
        }

        // inc the counters
        void incGenOnly(double w) { 
            num++; sumw += w; sumw2 += (w*w); 
        }
        void incLHE(double w0, const std::vector<double> & wScale, const std::vector<double> & wPDF, const std::vector<double> & wNamed) {
            // add up weights
            incGenOnly(w0);
            // then add up variations
            if (!wScale.empty()) {
                if (sumScale.empty()) sumScale.resize(wScale.size(), 0);
                for (unsigned int i = 0, n = wScale.size(); i < n; ++i) sumScale[i] += (w0 * wScale[i]);
            }
            if (!wPDF.empty()) {
                if (sumPDF.empty()) sumPDF.resize(wPDF.size(), 0);
                for (unsigned int i = 0, n = wPDF.size(); i < n; ++i) sumPDF[i] += (w0 * wPDF[i]);
            }
            if (!wNamed.empty()) {
                if (sumNamed.empty()) sumNamed.resize(wNamed.size(), 0);
                for (unsigned int i = 0, n = wNamed.size(); i < n; ++i) sumNamed[i] += (w0 * wNamed[i]);
            }
        }

        void merge(const Counter & other) { 
            num += other.num; sumw += other.sumw; sumw2 += other.sumw2; 
            if (sumScale.empty() && !other.sumScale.empty()) sumScale.resize(other.sumScale.size(),0);
            if (sumPDF.empty() && !other.sumPDF.empty()) sumPDF.resize(other.sumPDF.size(),0);
            if (sumNamed.empty() && !other.sumNamed.empty()) sumNamed.resize(other.sumNamed.size(),0);
            for (unsigned int i = 0, n = sumScale.size(); i < n; ++i) sumScale[i] += other.sumScale[i];
            for (unsigned int i = 0, n = sumPDF.size(); i < n; ++i) sumPDF[i] += other.sumPDF[i];
            for (unsigned int i = 0, n = sumNamed.size(); i < n; ++i) sumNamed[i] += other.sumNamed[i];
        }
    };

    ///  ---- RunCache object for dynamic choice of LHE IDs ----
    struct DynamicWeightChoice {
        // choice of LHE weights
        // ---- scale ----
        std::vector<std::string> scaleWeightIDs; 
        std::string scaleWeightsDoc;
        // ---- pdf ----
        std::vector<std::string> pdfWeightIDs; 
        std::string pdfWeightsDoc;
    };

    ///  -------------- temporary objects --------------
    struct ScaleVarWeight {
        std::string wid, label;
        std::pair<float,float> scales;
        ScaleVarWeight(const std::string & id, const std::string & text, const std::string & muR, const std::string & muF) :
            wid(id), label(text), scales(std::stof(muR), std::stof(muF)) {}
        bool operator<(const ScaleVarWeight & other) { return (scales == other.scales ? wid < other.wid : scales < other.scales); }
    };
    struct PDFSetWeights {
        std::vector<std::string> wids;
        std::pair<unsigned int,unsigned int> lhaIDs;
        PDFSetWeights(const std::string & wid, unsigned int lhaID) : wids(1,wid), lhaIDs(lhaID,lhaID) {}
        bool operator<(const PDFSetWeights & other) const { return lhaIDs < other.lhaIDs; }
        bool maybe_add(const std::string & wid, unsigned int lhaID) {
            if (lhaID == lhaIDs.second+1) {
                lhaIDs.second++;
                wids.push_back(wid);
                return true;
            } else {
                return false;
            }
        }
    };
}

class GenWeightsTableProducer : public edm::global::EDProducer<edm::StreamCache<Counter>, edm::RunCache<DynamicWeightChoice>, edm::RunSummaryCache<Counter>, edm::EndRunProducer> {
    public:
        GenWeightsTableProducer( edm::ParameterSet const & params ) :
            genTag_(consumes<GenEventInfoProduct>(params.getParameter<edm::InputTag>("genEvent"))),
            lheLabel_(params.getParameter<edm::InputTag>("lheInfo")),
            lheTag_(consumes<LHEEventProduct>(lheLabel_)),
            lheRunTag_(consumes<LHERunInfoProduct, edm::InRun>(lheLabel_)),
            preferredPDFLHAIDs_(params.getParameter<std::vector<uint32_t>>("preferredPDFs")),
            namedWeightIDs_(params.getParameter<std::vector<std::string>>("namedWeightIDs")),
            namedWeightLabels_(params.getParameter<std::vector<std::string>>("namedWeightLabels")),
            debug_(params.getUntrackedParameter<bool>("debug",false)), debugRun_(debug_.load()),
            hasIssuedWarning_(false)
        {
            produces<FlatTable>();
            produces<FlatTable>("LHEScale");
            produces<FlatTable>("LHEPdf");
            produces<FlatTable>("LHENamed");
            produces<MergableCounterTable,edm::InRun>();
            if (namedWeightIDs_.size() != namedWeightLabels_.size()) {
                throw cms::Exception("Configuration", "Size mismatch between namedWeightIDs & namedWeightLabels");
            }
        }

        virtual ~GenWeightsTableProducer() {}

        void produce(edm::StreamID id, edm::Event& iEvent, const edm::EventSetup& iSetup) const override {
            // get my counter for weights
            Counter * counter = streamCache(id);

            // generator information (always available)
            edm::Handle<GenEventInfoProduct> genInfo;
            iEvent.getByToken(genTag_, genInfo);
            double weight = genInfo->weight();
            
            // table for gen info, always available
            auto out = std::make_unique<FlatTable>(1, "genWeight", true);
            out->setDoc("generator weight");
            out->addColumnValue<float>("", weight, "generator weight", FlatTable::FloatColumn);
            iEvent.put(std::move(out));

            // tables for LHE weights, may not be filled
            std::unique_ptr<FlatTable> lheScaleTab, lhePdfTab, lheNamedTab;

            edm::Handle<LHEEventProduct> lheInfo;
            if (iEvent.getByToken(lheTag_, lheInfo)) {
                // get the dynamic choice of weights
                const DynamicWeightChoice * weightChoice = runCache(iEvent.getRun().index());
                // go fill tables
                fillLHEWeightTables(counter, weightChoice, weight, *lheInfo, lheScaleTab, lhePdfTab, lheNamedTab); 
            } else {
                // minimal book-keeping of weights
                counter->incGenOnly(weight);
                // make dummy values 
                lheScaleTab.reset(new FlatTable(1, "LHEScaleWeights", true));
                lhePdfTab.reset(new FlatTable(1, "LHEPdfWeights", true));
                lheNamedTab.reset(new FlatTable(1, "LHENamedWeights", true));
                if (!hasIssuedWarning_.exchange(false)) {
                    edm::LogWarning("LHETablesProducer") << "No LHEEventProduct, so there will be no LHE Tables\n";
                }
            }

            iEvent.put(std::move(lheScaleTab), "LHEScale");
            iEvent.put(std::move(lhePdfTab), "LHEPdf");
            iEvent.put(std::move(lheNamedTab), "LHENamed");
        }

        void fillLHEWeightTables(
                Counter * counter,
                const DynamicWeightChoice * weightChoice,
                double genWeight,
                const LHEEventProduct & lheProd, 
                std::unique_ptr<FlatTable> & outScale, 
                std::unique_ptr<FlatTable> & outPdf, 
                std::unique_ptr<FlatTable> & outNamed ) const 
        {
            bool lheDebug = debug_.exchange(false); // make sure only the first thread dumps out this (even if may still be mixed up with other output, but nevermind)

            const std::vector<std::string> & scaleWeightIDs = weightChoice->scaleWeightIDs;
            const std::vector<std::string> & pdfWeightIDs   = weightChoice->pdfWeightIDs;

            double w0 = lheProd.originalXWGTUP();

            std::vector<double> wScale(scaleWeightIDs.size(), 1), wPDF(pdfWeightIDs.size(), 1), wNamed(namedWeightIDs_.size(), 1);
            for (auto & weight : lheProd.weights()) {
                if (lheDebug) printf("Weight  %+9.5f   rel %+9.5f   for id %s\n", weight.wgt, weight.wgt/w0,  weight.id.c_str());
                // now we do it slowly, can be optimized
                auto mScale = std::find(scaleWeightIDs.begin(), scaleWeightIDs.end(), weight.id);
                if (mScale != scaleWeightIDs.end()) wScale[mScale-scaleWeightIDs.begin()] = weight.wgt/w0;

                auto mPDF = std::find(pdfWeightIDs.begin(), pdfWeightIDs.end(), weight.id);
                if (mPDF != pdfWeightIDs.end()) wPDF[mPDF-pdfWeightIDs.begin()] = weight.wgt/w0;

                auto mNamed = std::find(namedWeightIDs_.begin(), namedWeightIDs_.end(), weight.id);
                if (mNamed != namedWeightIDs_.end()) wNamed[mNamed-namedWeightIDs_.begin()] = weight.wgt/w0;
            } 

            outScale.reset(new FlatTable(wScale.size(), "LHEScaleWeight", false));
            outScale->addColumn<float>("", wScale, weightChoice->scaleWeightsDoc, FlatTable::FloatColumn); 

            outPdf.reset(new FlatTable(wPDF.size(), "LHEPdfWeight", false));
            outPdf->addColumn<float>("", wPDF, weightChoice->pdfWeightsDoc, FlatTable::FloatColumn); 

            outNamed.reset(new FlatTable(1, "LHEWeight", true));
            outNamed->addColumnValue<float>("originalXWGTUP", lheProd.originalXWGTUP(), "Nominal event weight in the LHE file", FlatTable::FloatColumn);
            for (unsigned int i = 0, n = wNamed.size(); i < n; ++i) {
                outNamed->addColumnValue<float>(namedWeightLabels_[i], wNamed[i], "LHE weight for id "+namedWeightIDs_[i]+", relative to nominal", FlatTable::FloatColumn);
            }
            
            counter->incLHE(genWeight, wScale, wPDF, wNamed);
        }

        // create an empty counter
        std::shared_ptr<DynamicWeightChoice> globalBeginRun(edm::Run const& iRun, edm::EventSetup const&) const override {
            edm::Handle<LHERunInfoProduct> lheInfo;

            bool lheDebug = debugRun_.exchange(false); // make sure only the first thread dumps out this (even if may still be mixed up with other output, but nevermind)
            auto weightChoice = std::make_shared<DynamicWeightChoice>();

            // getByToken throws since we're not in the endRun (see https://github.com/cms-sw/cmssw/pull/18499)
            //if (iRun.getByToken(lheRunTag_, lheInfo)) {
            if (iRun.getByLabel(lheLabel_, lheInfo)) { 
                std::vector<ScaleVarWeight> scaleVariationIDs;
                std::vector<PDFSetWeights>  pdfSetWeightIDs;
                
                std::regex weightgroup("<weightgroup\\s+combine=\"(.*)\"\\s+name=\"(.*)\"\\s*>");
                std::regex endweightgroup("</weightgroup>");
                std::regex scalew("<weight\\s+id=\"(\\d+)\">\\s*(muR=(\\S+)\\s+muF=(\\S+)(\\s+.*)?)</weight>");
                std::regex pdfw("<weight\\s+id=\"(\\d+)\">\\s*PDF set\\s*=\\s*(\\d+)\\s*</weight>");
                std::smatch groups;
                for (auto iter=lheInfo->headers_begin(), end = lheInfo->headers_end(); iter != end; ++iter) {
                    if (iter->tag() != "initrwgt") {
                        if (lheDebug) std::cout << "Skipping LHE header with tag" << iter->tag() << std::endl;
                        continue;
                    }
                    if (lheDebug) std::cout << "Found LHE header with tag" << iter->tag() << std::endl;
                    const std::vector<std::string> & lines = iter->lines();
                    for (unsigned int iLine = 0, nLines = lines.size(); iLine < nLines; ++iLine) {
                        if (lheDebug) std::cout << lines[iLine];
                        if (std::regex_search(lines[iLine], groups, weightgroup)) {
                            if (lheDebug) std::cout << ">>> Looks like the beginning of a weight group for " << groups.str(2) << std::endl;
                            if (groups.str(2) == "scale_variation") {
                                for ( ++iLine; iLine < nLines; ++iLine) {
                                    if (lheDebug) std::cout << "    " << lines[iLine];
                                    if (std::regex_search(lines[iLine], groups, scalew)) {
                                        if (lheDebug) std::cout << "    >>> Scale weight " << groups[1].str() << " for " << groups[3].str() << " , " << groups[4].str() << " , " << groups[5].str() << std::endl;
                                        scaleVariationIDs.emplace_back(groups.str(1), groups.str(2), groups.str(3), groups.str(4));
                                    } else if (std::regex_search(lines[iLine], endweightgroup)) {
                                        if (lheDebug) std::cout << ">>> Looks like the end of a weight group" << std::endl;
                                        break;
                                    } else if (std::regex_search(lines[iLine], weightgroup)) {
                                        if (lheDebug) std::cout << ">>> Looks like the beginning of a new weight group, I will assume I missed the end of the group." << std::endl;
                                        --iLine; // rewind by one, and go back to the outer loop
                                        break;
                                    }
                                }
                            } else if (groups.str(2) == "PDF_variation") {
                                for ( ++iLine; iLine < nLines; ++iLine) {
                                    if (lheDebug) std::cout << "    " << lines[iLine];
                                    if (std::regex_search(lines[iLine], groups, pdfw)) {
                                        unsigned int lhaID = std::stoi(groups.str(2));
                                        if (lheDebug) std::cout << "    >>> PDF weight " << groups.str(1) << " for " << groups.str(2) << " = " << lhaID << std::endl;
                                        if (pdfSetWeightIDs.empty() || ! pdfSetWeightIDs.back().maybe_add(groups.str(1),lhaID)) {
                                            pdfSetWeightIDs.emplace_back(groups.str(1),lhaID);
                                        }
                                    } else if (std::regex_search(lines[iLine], endweightgroup)) {
                                        if (lheDebug) std::cout << ">>> Looks like the end of a weight group" << std::endl;
                                        break;
                                    } else if (std::regex_search(lines[iLine], weightgroup)) {
                                        if (lheDebug) std::cout << ">>> Looks like the beginning of a new weight group, I will assume I missed the end of the group." << std::endl;
                                        --iLine; // rewind by one, and go back to the outer loop
                                        break;
                                    }
                                }
                            } else {
                                for ( ++iLine; iLine < nLines; ++iLine) {
                                    if (lheDebug) std::cout << "    " << lines[iLine];
                                    if (std::regex_search(lines[iLine], groups, endweightgroup)) {
                                        if (lheDebug) std::cout << ">>> Looks like the end of a weight group" << std::endl;
                                        break;
                                    } else if (std::regex_search(lines[iLine], weightgroup)) {
                                        if (lheDebug) std::cout << ">>> Looks like the beginning of a new weight group, I will assume I missed the end of the group." << std::endl;
                                        --iLine; // rewind by one, and go back to the outer loop
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    //std::cout << "============= END [ " << iter->tag() << " ] ============ \n\n" << std::endl;

                    // ----- SCALE VARIATIONS -----
                    std::sort(scaleVariationIDs.begin(), scaleVariationIDs.end());
                    if (lheDebug) std::cout << "Found " << scaleVariationIDs.size() << " scale variations: " << std::endl;
                    std::stringstream scaleDoc("LHE scale variation weights (w_var / w_nominal); ");
                    for (unsigned int isw = 0, nsw = scaleVariationIDs.size(); isw < nsw; ++isw) {
                        const auto & sw = scaleVariationIDs[isw];
                        if (isw) scaleDoc << "; ";
                        scaleDoc << "[" << isw << "] is " << sw.label;
                        weightChoice->scaleWeightIDs.push_back(sw.wid);
                        if (lheDebug) printf("    id %s: scales ren = % .2f  fact = % .2f  text = %s\n", sw.wid.c_str(), sw.scales.first, sw.scales.second, sw.label.c_str());
                    }
                    if (!scaleVariationIDs.empty()) weightChoice->scaleWeightsDoc = scaleDoc.str();

                    // ------ PDF VARIATIONS (take the preferred one) -----
                    if (lheDebug) {
                        std::cout << "Found " << pdfSetWeightIDs.size() << " PDF set errors: " << std::endl;
                        for (const auto & pw : pdfSetWeightIDs) printf("lhaIDs %6d - %6d (%3lu weights: %s, ... )\n", pw.lhaIDs.first, pw.lhaIDs.second, pw.wids.size(), pw.wids.front().c_str());
                    }
                    
                    std::stringstream pdfDoc("LHE pdf variation weights (w_var / w_nominal) for LHA IDs ");
                    bool found = false;
                    for (uint32_t lhaid : preferredPDFLHAIDs_) {
                        for (const auto & pw : pdfSetWeightIDs) {
                            if (pw.lhaIDs.first != lhaid) continue;
                            pdfDoc << pw.lhaIDs.first << " - " << pw.lhaIDs.second << std::endl;
                            weightChoice->pdfWeightIDs = pw.wids;
                            weightChoice->pdfWeightsDoc = pdfDoc.str(); 
                            found = true; break;
                        }
                        if (found) break;
                    }
                }
            }
            return weightChoice; 
        }


        // create an empty counter
        std::unique_ptr<Counter> beginStream(edm::StreamID) const override { 
            return std::make_unique<Counter>(); 
        }
        // inizialize to zero at begin run
        void streamBeginRun(edm::StreamID id, edm::Run const&, edm::EventSetup const&) const override { 
            streamCache(id)->clear(); 
        }
        // create an empty counter
        std::shared_ptr<Counter> globalBeginRunSummary(edm::Run const&, edm::EventSetup const&) const override { 
            return std::make_shared<Counter>(); 
        }
        // add this stream to the summary
        void streamEndRunSummary(edm::StreamID id, edm::Run const&, edm::EventSetup const&, Counter* runCounter) const override { 
            runCounter->merge(*streamCache(id)); 
        }
        // nothing to do per se
        void globalEndRunSummary(edm::Run const&, edm::EventSetup const&, Counter* runCounter) const override { 
        }
        // write the total to the run 
        void globalEndRunProduce(edm::Run& iRun, edm::EventSetup const&, Counter const* runCounter) const override {
            auto out = std::make_unique<MergableCounterTable>();
            out->addInt("genEventCount", "event count", runCounter->num);
            out->addFloat("genEventSumw", "sum of gen weights", runCounter->sumw);
            out->addFloat("genEventSumw2", "sum of gen (weight^2)", runCounter->sumw2);

            out->addVFloat("LHEScaleSumw", "Sum of genEventWeight * LHEScaleWeight[i]", runCounter->sumScale);
            out->addVFloat("LHEPdfSumw", "Sum of genEventWeight * LHEPdfWeight[i]", runCounter->sumPDF);
            if (!runCounter->sumNamed.empty()) { // it could be empty if there's no LHE info in the sample
                for (unsigned int i = 0, n = namedWeightLabels_.size(); i < n; ++i) {
                    out->addFloat("LHESumw_"+namedWeightLabels_[i], "Sum of genEventWeight * LHEWeight_"+namedWeightLabels_[i], runCounter->sumNamed[i]);
                }
            }
            iRun.put(std::move(out));
        }
        // nothing to do here
        void globalEndRun(edm::Run const&, edm::EventSetup const&) const override { }
    protected:
        const edm::EDGetTokenT<GenEventInfoProduct> genTag_;
        const edm::InputTag lheLabel_;
        const edm::EDGetTokenT<LHEEventProduct> lheTag_;
        const edm::EDGetTokenT<LHERunInfoProduct> lheRunTag_;

        std::vector<uint32_t> preferredPDFLHAIDs_;
        std::vector<std::string> namedWeightIDs_;
        std::vector<std::string> namedWeightLabels_;

        mutable std::atomic<bool> debug_, debugRun_, hasIssuedWarning_;
};

#include "FWCore/Framework/interface/MakerMacros.h"
DEFINE_FWK_MODULE(GenWeightsTableProducer);

