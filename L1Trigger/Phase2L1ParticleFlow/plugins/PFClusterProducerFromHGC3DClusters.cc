#include "FWCore/Framework/interface/Frameworkfwd.h"
#include "FWCore/Framework/interface/stream/EDProducer.h"
#include "FWCore/Framework/interface/Event.h"
#include "FWCore/Framework/interface/MakerMacros.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"

#include "DataFormats/L1TParticleFlow/interface/PFCluster.h"
#include "L1Trigger/Phase2L1ParticleFlow/src/corrector.h"
#include "L1Trigger/Phase2L1ParticleFlow/interface/ParametricResolution.h"
#include "L1Trigger/Phase2L1ParticleFlow/interface/HGC3DClusterEgID.h"
#include "DataFormats/L1THGCal/interface/HGCalMulticluster.h"
#include "CommonTools/Utils/interface/StringCutObjectSelector.h"

namespace l1tpf {
  class PFClusterProducerFromHGC3DClusters : public edm::stream::EDProducer<> {
  public:
    explicit PFClusterProducerFromHGC3DClusters(const edm::ParameterSet &);
    ~PFClusterProducerFromHGC3DClusters() override {}

  private:
    edm::EDGetTokenT<l1t::HGCalMulticlusterBxCollection> src_;
    int scenario_;
    bool emOnly_;
    double etCut_;
    StringCutObjectSelector<l1t::HGCalMulticluster> preEmId_;
    l1tpf::HGC3DClusterEgID emVsPionID_, emVsPUID_;
    bool hasEmId_;
    l1tpf::corrector corrector_;
    l1tpf::ParametricResolution resol_;

    void produce(edm::Event &, const edm::EventSetup &) override;

  };  // class
}  // namespace l1tpf

l1tpf::PFClusterProducerFromHGC3DClusters::PFClusterProducerFromHGC3DClusters(const edm::ParameterSet &iConfig)
    : src_(consumes<l1t::HGCalMulticlusterBxCollection>(iConfig.getParameter<edm::InputTag>("src"))),
      scenario_(iConfig.getParameter<int>("scenario")),
      emOnly_(iConfig.getParameter<bool>("emOnly")),
      etCut_(iConfig.getParameter<double>("etMin")),
      preEmId_(iConfig.getParameter<std::string>("preEmId")),
      emVsPionID_(iConfig.getParameter<edm::ParameterSet>("emVsPionID")),
      emVsPUID_(iConfig.getParameter<edm::ParameterSet>("emVsPUID")),
      hasEmId_((iConfig.existsAs<std::string>("preEmId") && !iConfig.getParameter<std::string>("preEmId").empty()) ||
               !emVsPionID_.method().empty()),
      corrector_(iConfig.getParameter<std::string>("corrector"),
                 emOnly_ || iConfig.getParameter<std::string>("corrector").empty()
                     ? -1
                     : iConfig.getParameter<double>("correctorEmfMax")),
      resol_(iConfig.getParameter<edm::ParameterSet>("resol")) {

  if (!emVsPionID_.method().empty()) {
    emVsPionID_.prepareTMVA();
  }
  if (!emVsPUID_.method().empty()) {
    emVsPUID_.prepareTMVA();
  }

  produces<l1t::PFClusterCollection>();
  if (hasEmId_) {
    produces<l1t::PFClusterCollection>("em");
    produces<l1t::PFClusterCollection>("had");
  }
}

void l1tpf::PFClusterProducerFromHGC3DClusters::produce(edm::Event &iEvent, const edm::EventSetup &) {
  auto out = std::make_unique<l1t::PFClusterCollection>();
  std::unique_ptr<l1t::PFClusterCollection> outEm, outHad;
  if (hasEmId_) {
    outEm.reset(new l1t::PFClusterCollection());
    outHad.reset(new l1t::PFClusterCollection());
  }
  edm::Handle<l1t::HGCalMulticlusterBxCollection> multiclusters;
  iEvent.getByToken(src_, multiclusters);

  for (auto it = multiclusters->begin(0), ed = multiclusters->end(0); it != ed; ++it) {
    float pt = it->pt(), hoe = it->hOverE();
    bool isEM = hasEmId_ ? preEmId_(*it) : emOnly_;
    if (emOnly_) {
      if (hoe == -1)
        continue;
      pt /= (1 + hoe);
      hoe = 0;
    }
    if (pt <= etCut_)
      continue;

    if (it->hwQual()) {  // this is the EG ID shipped with the HGC TPs
      // we use the EM interpretation of the cluster energy
      l1t::PFCluster egcluster(
          it->iPt(l1t::HGCalMulticluster::EnergyInterpretation::EM), it->eta(), it->phi(), hoe, false);
      // NOTE: we use HW qual = 4 to identify the EG candidates and set isEM to false not to interfere with the rest of the PF...
      // we start from 4 not to intefere with flags used elesewhere
      egcluster.setHwQual(4);
      egcluster.addConstituent(edm::Ptr<l1t::L1Candidate>(multiclusters, multiclusters->key(it)));
      outEm->push_back(egcluster);
    }

    l1t::PFCluster cluster(pt, it->eta(), it->phi(), hoe, /*isEM=*/isEM);
    if (!emVsPUID_.method().empty()) {
      if (!emVsPUID_.passID(*it, cluster)) {
        continue;
      }
    }
    if (!emVsPionID_.method().empty()) {
      isEM = emVsPionID_.passID(*it, cluster);
      cluster.setIsEM(isEM);

      if (scenario_ == 1){
	if (isEM){
	  float pt_tmp  = it->iPt(l1t::HGCalMulticluster::EnergyInterpretation::EM);
	  float hoe_tmp = 0.;
	  l1t::PFCluster cluster_tmp(pt_tmp, it->eta(), it->phi(), hoe_tmp, /*isEM=*/isEM);
	  cluster = cluster_tmp;
	}
      }
      else if (scenario_ == 2){
	float em_part = pt/(1+hoe);
	float pt_tmp  = it->iPt(l1t::HGCalMulticluster::EnergyInterpretation::EM) + hoe * em_part;
	float hoe_tmp = hoe * em_part / it->iPt(l1t::HGCalMulticluster::EnergyInterpretation::EM);
	l1t::PFCluster cluster_tmp(pt_tmp, it->eta(), it->phi(), hoe_tmp, /*isEM=*/isEM);	
	cluster = cluster_tmp;
      }
    }
    if (corrector_.valid())
      corrector_.correctPt(cluster);
    cluster.setPtError(resol_(cluster.pt(), std::abs(cluster.eta())));

    out->push_back(cluster);
    out->back().addConstituent(edm::Ptr<l1t::L1Candidate>(multiclusters, multiclusters->key(it)));
    if (hasEmId_) {
      (isEM ? outEm : outHad)->push_back(out->back());
    }
  }

  iEvent.put(std::move(out));
  if (hasEmId_) {
    iEvent.put(std::move(outEm), "em");
    iEvent.put(std::move(outHad), "had");
  }
}
using l1tpf::PFClusterProducerFromHGC3DClusters;
DEFINE_FWK_MODULE(PFClusterProducerFromHGC3DClusters);
