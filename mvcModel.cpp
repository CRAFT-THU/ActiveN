#include "modelSpec.h"
void modelDefinition(ModelSpec &model)
{
  model.setDT(0.1);
  model.setName("mvc");
  NeuronModels::LIF::ParamValues lifs_p(
    1,
    1, // TODO: modify me
    0,
    0,
    15,
    0,
    0
  );
  NeuronModels::LIF::VarValues lifs_ini(
    uninitialisedVar(),
    0
  );
  model.addNeuronPopulation<NeuronModels::LIF>("lifs", 82469, lifs_p, lifs_ini);

  WeightUpdateModels::StaticPulse::VarValues s_ini(uninitialisedVar());
  PostsynapticModels::DeltaCurr::ParamValues ps_p;
  auto grp = model.addSynapsePopulation<WeightUpdateModels::StaticPulse, PostsynapticModels::DeltaCurr>(
    "syn", SynapseMatrixType::SPARSE_INDIVIDUALG, NO_DELAY, //FIXME: change
    "lifs", "lifs",
    {}, s_ini,
    ps_p, {}
  );
  grp->setMaxConnections(476);
}
