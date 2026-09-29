import json
import unittest
from unittest.mock import patch
import numpy as np
import brunel_checkpoint as checkpoint
from brunel import constants


def fixture():
    d={"metadata":np.array(json.dumps(dict(format="brunel-genn-phase-checkpoint-v1",
         boundary="after-synapses-before-neurons",rule="morrison",dt_ms=.1,
         timestep=200,delay_steps=0)))}
    for group in ("exc","inh"):
        for key in ("V","Iex","dIex","Iin","dIin"):
            d[f"{group}_{key}"]=np.array([2.,3.],np.float32)
        d[f"{group}_refrac"]=np.array([0,4],np.uint32)
        d[f"{group}_tick"]=np.full(2,200,np.uint32)
    for group,degree in (("ee",450),("ei",450),("ie",112),("ii",112)):
        d[f"{group}_post"]=np.repeat([0,1],degree).astype(np.int32)
        d[f"{group}_pre"]=1-d[f"{group}_post"]
        d[f"{group}_input"]=np.array([[11.,17.]],np.float32)
    d["ee_g"]=np.linspace(10,20,900).astype(np.float32)
    d["canonical_pre_trace"]=np.array([.25,.75],np.float32)
    d["canonical_post_trace"]=np.array([.5,1.5],np.float32)
    return d


class CheckpointImport(unittest.TestCase):
    def convert(self,data):
        with patch.object(checkpoint,"NE",2),patch.object(checkpoint,"NI",2):
            return checkpoint.convert(data,"morrison",20260724)

    def test_state_currents_traces_and_parallel_edge_weights(self):
        data=fixture()
        edges,(nodes,weights),meta=self.convert(data)
        f=nodes.view(np.float32)
        np.testing.assert_array_equal(f[:2,6],data["canonical_pre_trace"])
        np.testing.assert_array_equal(f[:2,7],data["canonical_post_trace"])
        np.testing.assert_array_equal(f[:,17], [11,17,11,17])
        np.testing.assert_array_equal(f[:,18], [11,17,11,17])
        np.testing.assert_array_equal(nodes[:,5], [0,4,0,4])
        self.assertTrue(np.all(nodes[:,9:11]==0))
        c=constants("morrison")
        expected=np.float32(data["ee_g"].astype(float)[np.argsort(data["ee_pre"],kind="stable")]
                            *float(c["epsc"])*float(c["delivery"]))
        np.testing.assert_array_equal(weights,expected)
        self.assertEqual(len(weights),900)
        self.assertTrue(np.all(np.diff(edges[0])>=0))
        self.assertTrue(np.all(np.diff(edges[2])>=0))
        self.assertEqual(meta["timestep"],200)

    def test_reject_bad_boundary_clock_or_graph(self):
        for kind in ("clock","graph","trace"):
            data=fixture()
            if kind=="clock":data["exc_tick"][0]=199
            if kind=="graph":data["ee_pre"][0]=0
            if kind=="trace":data["canonical_post_trace"][0]=np.nan
            with self.assertRaises(ValueError):self.convert(data)


if __name__=="__main__":unittest.main()
