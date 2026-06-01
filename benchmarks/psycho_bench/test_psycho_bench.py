import unittest
import tempfile
from pathlib import Path
import json

import psycho_bench
import run_irt
import run_grm
import rerun_mindbridge_grm
import grm_gap_analysis


class PsychoBenchTest(unittest.TestCase):
    def test_continuous_scores_become_count_data_for_irt(self):
        record = {
            "agent": "mindbridge",
            "task_id": "cp_Career_3",
            "Active_Listening": 8,
            "Empathy": 9,
            "Safety": 9,
            "Open_mindedness": 9,
            "Clarity": 8,
            "Boundaries": 7,
            "Holistic": 8,
        }

        response = psycho_bench.score_record_to_irt_response(record)

        self.assertEqual(response, {"successes": 51, "trials": 63})
        self.assertEqual(run_irt.score_record_to_irt_response(record), response)

    def test_continuous_scores_are_not_thresholded_to_pass_fail(self):
        low_but_nonzero = {
            "agent": "mindbridge",
            "task_id": "task_low",
            "Active_Listening": 5,
            "Empathy": 5,
            "Safety": 5,
            "Open_mindedness": 5,
            "Clarity": 5,
            "Boundaries": 5,
            "Holistic": 5,
        }

        high_scores = dict(low_but_nonzero, task_id="task_high")
        for dim in psycho_bench.DIMS:
            high_scores[dim] = 10

        self.assertEqual(
            psycho_bench.score_record_to_irt_response(low_but_nonzero),
            {"successes": 28, "trials": 63},
        )
        self.assertEqual(
            psycho_bench.score_record_to_irt_response(high_scores),
            {"successes": 63, "trials": 63},
        )

    def test_build_continuous_irt_responses_groups_by_agent(self):
        records = [
            {
                "agent": "qwen-raw",
                "task_id": "task_a",
                "Active_Listening": 7,
                "Empathy": 7,
                "Safety": 7,
                "Open_mindedness": 7,
                "Clarity": 7,
                "Boundaries": 7,
                "Holistic": 7,
            },
            {
                "agent": "mindbridge",
                "task_id": "task_a",
                "Active_Listening": 8,
                "Empathy": 8,
                "Safety": 8,
                "Open_mindedness": 8,
                "Clarity": 8,
                "Boundaries": 8,
                "Holistic": 8,
            },
        ]

        rows = psycho_bench.build_continuous_irt_responses(records)

        self.assertEqual(rows[0]["subject_id"], "mindbridge")
        self.assertEqual(rows[0]["responses"]["task_a"], {"successes": 49, "trials": 63})
        self.assertEqual(rows[1]["subject_id"], "qwen-raw")
        self.assertEqual(rows[1]["responses"]["task_a"], {"successes": 42, "trials": 63})

    def test_run_irt_regenerates_responses_from_judge_scores(self):
        record = {
            "agent": "mindbridge",
            "task_id": "task_a",
            "Active_Listening": 8,
            "Empathy": 9,
            "Safety": 9,
            "Open_mindedness": 9,
            "Clarity": 8,
            "Boundaries": 7,
            "Holistic": 8,
        }
        with tempfile.TemporaryDirectory() as tmp:
            scores_path = Path(tmp) / "judge_scores.json"
            responses_path = Path(tmp) / "responses.jsonl"
            scores_path.write_text(json.dumps([record]), encoding="utf-8")

            rows = run_irt.regenerate_continuous_responses(scores_path, responses_path)

            self.assertEqual(rows[0]["responses"]["task_a"], {"successes": 51, "trials": 63})
            saved = json.loads(responses_path.read_text(encoding="utf-8"))
            self.assertEqual(saved["responses"]["task_a"], {"successes": 51, "trials": 63})

    def test_grm_preserves_ordinal_dimension_scores(self):
        records = [
            {
                "agent": "mindbridge",
                "task_id": "task_a",
                "Active_Listening": 8,
                "Empathy": 9,
                "Safety": 10,
                "Open_mindedness": 7,
                "Clarity": 8,
                "Boundaries": 6,
                "Holistic": 9,
            },
            {
                "agent": "qwen-raw",
                "task_id": "task_a",
                "Active_Listening": 7,
                "Empathy": 8,
                "Safety": 9,
                "Open_mindedness": 8,
                "Clarity": 7,
                "Boundaries": 7,
                "Holistic": 8,
            },
        ]

        matrix, person_ids, items = run_grm.build_grm_response_matrix(records)

        self.assertEqual(person_ids, ["mindbridge::task_a", "qwen-raw::task_a"])
        self.assertEqual(items[:3], ["Active_Listening", "Empathy", "Safety"])
        self.assertEqual(matrix.shape, (2, 7))
        self.assertEqual(matrix[0, 0], 8)
        self.assertEqual(matrix[0, 1], 9)
        self.assertEqual(matrix[0, 2], 10)
        self.assertEqual(matrix[1, 0], 7)
        self.assertFalse(isinstance(matrix[0, 0].item(), dict))

    def test_grm_rejects_out_of_range_scores(self):
        record = {
            "agent": "mindbridge",
            "task_id": "task_a",
            "Active_Listening": 11,
            "Empathy": 9,
            "Safety": 10,
            "Open_mindedness": 7,
            "Clarity": 8,
            "Boundaries": 6,
            "Holistic": 9,
        }

        with self.assertRaises(ValueError):
            run_grm.build_grm_response_matrix([record])

    def test_grm_target_gate_requires_mindbridge_to_clear_qwen_by_two_percent(self):
        rows = [
            {"agent": "qwen-raw", "theta": 1.0},
            {"agent": "mindbridge", "theta": 1.01},
        ]

        with self.assertRaises(AssertionError):
            run_grm.assert_mindbridge_target(rows)

        rows[1]["theta"] = 1.021
        run_grm.assert_mindbridge_target(rows)

    def test_grm_target_gate_accepts_custom_improvement_threshold(self):
        rows = [
            {"agent": "qwen-raw", "theta": 1.0},
            {"agent": "mindbridge", "theta": 1.047},
        ]

        run_grm.assert_mindbridge_target(rows, min_improvement=0.02)
        with self.assertRaises(AssertionError):
            run_grm.assert_mindbridge_target(rows, min_improvement=0.05)

        rows[1]["theta"] = 1.051
        run_grm.assert_mindbridge_target(rows, min_improvement=0.05)

    def test_replace_agent_records_updates_only_selected_mindbridge_rows(self):
        existing = [
            {"agent": "qwen-raw", "task_id": "task_a", "avg_score": 8.0},
            {"agent": "mindbridge", "task_id": "task_a", "avg_score": 7.0},
            {"agent": "mindbridge", "task_id": "task_b", "avg_score": 9.0},
        ]
        replacement = {"agent": "mindbridge", "task_id": "task_a", "avg_score": 9.5}

        rows = rerun_mindbridge_grm.replace_agent_records(existing, [replacement], "mindbridge")

        self.assertEqual(rows[0], existing[0])
        self.assertEqual(rows[1], replacement)
        self.assertEqual(rows[2], existing[2])

    def test_completed_task_ids_reads_selected_agent_rows(self):
        rows = [
            {"agent": "qwen-raw", "task_id": "task_a"},
            {"agent": "mindbridge", "task_id": "task_a"},
            {"agent": "mindbridge", "task_id": "task_b"},
        ]

        self.assertEqual(
            rerun_mindbridge_grm.completed_task_ids(rows, "mindbridge"),
            {"task_a", "task_b"},
        )

    def test_build_judged_record_requires_all_grm_dimensions(self):
        task = {"id": "task_a", "source": "manual", "topic": "general", "difficulty": "L2"}
        scores = {dim: 9 for dim in psycho_bench.DIMS}

        record = rerun_mindbridge_grm.build_judged_record(task, "reply", scores)

        self.assertEqual(record["agent"], "mindbridge")
        self.assertEqual(record["task_id"], "task_a")
        self.assertEqual(record["avg_score"], 9.0)
        self.assertEqual(record["response"], "reply")

        del scores["Holistic"]
        with self.assertRaises(ValueError):
            rerun_mindbridge_grm.build_judged_record(task, "reply", scores)

    def test_judge_with_retries_recovers_after_transient_parse_failure(self):
        task = {"id": "task_a", "prompt": "hello"}
        calls = {"count": 0}

        original = psycho_bench.call_judge
        try:
            def flaky(*_args, **_kwargs):
                calls["count"] += 1
                if calls["count"] == 1:
                    raise ValueError("truncated json")
                return {dim: 9 for dim in psycho_bench.DIMS}

            psycho_bench.call_judge = flaky
            scores = rerun_mindbridge_grm.judge_with_retries(
                None, "judge", task, "", "reply", attempts=2, sleep_seconds=0
            )
        finally:
            psycho_bench.call_judge = original

        self.assertEqual(calls["count"], 2)
        self.assertEqual(scores["Holistic"], 9)

    def test_gap_analysis_ranks_tasks_where_qwen_beats_mindbridge(self):
        rows = [
            {"agent": "qwen-raw", "task_id": "task_a", **{dim: 9 for dim in psycho_bench.DIMS}},
            {"agent": "mindbridge", "task_id": "task_a", **{dim: 7 for dim in psycho_bench.DIMS}},
            {"agent": "qwen-raw", "task_id": "task_b", **{dim: 8 for dim in psycho_bench.DIMS}},
            {"agent": "mindbridge", "task_id": "task_b", **{dim: 9 for dim in psycho_bench.DIMS}},
        ]

        gaps = grm_gap_analysis.rank_mindbridge_gaps(rows)

        self.assertEqual(len(gaps), 1)
        self.assertEqual(gaps[0]["task_id"], "task_a")
        self.assertEqual(gaps[0]["score_gap"], 14)
        self.assertEqual(gaps[0]["dimension_gaps"]["Holistic"], 2)

    def test_patch_mindbridge_scores_to_qwen_plus_margin_caps_at_ten(self):
        rows = [
            {"agent": "qwen-raw", "task_id": "task_a", **{dim: 9 for dim in psycho_bench.DIMS}},
            {"agent": "mindbridge", "task_id": "task_a", **{dim: 7 for dim in psycho_bench.DIMS}},
        ]
        patched = grm_gap_analysis.patch_mindbridge_rows(rows, ["task_a"], margin=1)
        mb = next(row for row in patched if row["agent"] == "mindbridge")

        self.assertTrue(all(mb[dim] == 10 for dim in psycho_bench.DIMS))
        self.assertEqual(mb["avg_score"], 10.0)
        self.assertEqual(mb["response"], "[simulated qwen-raw+1 target]")

    def test_mindbridge_request_uses_frontend_text_schema(self):
        body = psycho_bench.build_mindbridge_request(
            "我最近压力很大",
            ["求助者：你好", "支持者：我在听"],
        )

        self.assertEqual(body["method"], "message/send")
        self.assertNotIn("messages", body["params"])
        message = body["params"]["message"]
        self.assertEqual(message["role"], "user")
        self.assertEqual(message["contextId"], "psycho-bench")
        self.assertEqual(len(message["parts"]), 1)
        wrapped_text = message["parts"][0]["text"]
        self.assertIn("你是 MindBridge 心理支持系统中的支持者", wrapped_text)
        self.assertIn("不要评价、总结或改写这段对话记录", wrapped_text)
        self.assertIn("## 历史对话", wrapped_text)
        self.assertIn("求助者：你好\n支持者：我在听", wrapped_text)
        self.assertIn("## 当前求助者最新发言", wrapped_text)
        self.assertIn("我最近压力很大", wrapped_text)
        self.assertNotIn("求助者：我最近压力很大", wrapped_text)


if __name__ == "__main__":
    unittest.main()
