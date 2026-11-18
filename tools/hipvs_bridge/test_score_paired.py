import unittest
from score_paired import summarize


def fixture():
    def report():
        return {"baseline": "base", "candidate": "cand", "raw": {
            "base": {str(k): {"build_ms": 30000, "qps": 100, "recall": 0.99} for k in (10, 100)},
            "cand": {str(k): {"build_ms": 10000, "qps": 150, "recall": 0.99} for k in (10, 100)},
        }}
    return {"sift": report(), "hidden_proxy": report()}


class ScoringTest(unittest.TestCase):
    def test_caps_are_conditional(self):
        result = summarize(fixture())
        self.assertEqual(result["score_if_all_other_gates_pass"], 20)
        self.assertIsNone(result["official_score"])

    def test_recall_delta_zeroes_score(self):
        reports = fixture()
        reports["sift"]["raw"]["cand"]["10"]["recall"] = 0.98
        result = summarize(reports)
        self.assertEqual(result["score_if_all_other_gates_pass"], 0)
        self.assertIn("sift:recall@10_baseline_delta", result["measured_gate_failures"])

    def test_per_dataset_query_gate(self):
        reports = fixture()
        for k in ("10", "100"):
            reports["sift"]["raw"]["cand"][k]["qps"] = 89
        result = summarize(reports)
        self.assertEqual(result["score_if_all_other_gates_pass"], 0)

    def test_weighted_geometric_mean(self):
        reports = fixture()
        for k in ("10", "100"):
            reports["sift"]["raw"]["cand"][k]["build_ms"] = 15000
        result = summarize(reports)
        self.assertAlmostEqual(result["build_speedup"], 2 ** 0.4 * 3 ** 0.6)

    def test_invalid_data(self):
        for value in (0, -1, float("nan"), float("inf")):
            reports = fixture()
            reports["sift"]["raw"]["cand"]["10"]["qps"] = value
            with self.assertRaises(ValueError):
                summarize(reports)
        reports = fixture()
        del reports["sift"]["raw"]["cand"]["100"]
        with self.assertRaises(KeyError):
            summarize(reports)


if __name__ == "__main__":
    unittest.main()
