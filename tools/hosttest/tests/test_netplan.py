import copy
import unittest

from hosttest import netplan

GOOD = {
    "nodes": [
        {"address": "0x0001", "role": "command"},
        {"address": "0x0002", "role": "command"},
        {"address": "0x0010", "role": "member"},
        {"address": "0xEFFF", "role": "member"},
    ],
    "groups": [
        {"address": "0xF000", "ip": "239.1.1.1"},
        {"address": "0xFFFD", "ip": "ff02::1:2"},
    ],
    "rf_heads": [
        {"name": "head0", "eui64": "00:11:22:33:44:55:66:77"},
        {"name": "head1", "eui64": "0011223344556688"},
    ],
    "nets": [
        {"net": "alpha", "rf_head": "head0", "net_id": "0x10"},
        {"net": "alpha", "rf_head": "head1", "net_id": 17},
    ],
}


def paths(plan):
    return [i.path for i in netplan.validate(plan)]


def changed(mutate):
    plan = copy.deepcopy(GOOD)
    mutate(plan)
    return plan


class ValidPlanTest(unittest.TestCase):
    def test_good_plan_has_no_issues(self):
        self.assertEqual(netplan.validate(GOOD), [])

    def test_empty_plan_is_valid(self):
        self.assertEqual(netplan.validate(netplan.empty_plan()), [])

    def test_non_object_plan_is_rejected(self):
        self.assertEqual(paths([]), ["plan"])


class NodeTest(unittest.TestCase):
    def test_range_edges(self):
        for bad in ("0x0000", "0xF000", "0x10000", -1, "abc", "", None, True, 1.5):
            plan = changed(lambda p, b=bad: p["nodes"][2].update(address=b))
            self.assertEqual(paths(plan), ["nodes[2].address"], bad)

    def test_duplicate_address(self):
        plan = changed(lambda p: p["nodes"][3].update(address="0x0010"))
        self.assertEqual(paths(plan), ["nodes[3].address"])

    def test_decimal_and_hex_are_the_same_address(self):
        plan = changed(lambda p: p["nodes"][3].update(address=16))
        self.assertEqual(paths(plan), ["nodes[3].address"])

    def test_bad_role(self):
        plan = changed(lambda p: p["nodes"][0].update(role="boss"))
        self.assertEqual(paths(plan), ["nodes[0].role"])

    def test_command_radio_above_a_member_is_rejected(self):
        plan = changed(lambda p: p["nodes"][1].update(address="0x0020"))
        self.assertEqual(paths(plan), ["nodes"])

    def test_rule_needs_both_roles_to_apply(self):
        plan = changed(lambda p: p.update(nodes=[
            {"address": "0x0900", "role": "command"}]))
        self.assertEqual(netplan.validate(plan), [])

    def test_row_must_be_an_object(self):
        plan = changed(lambda p: p["nodes"].append("0x0020"))
        self.assertEqual(paths(plan), ["nodes[4]"])

    def test_list_required(self):
        plan = changed(lambda p: p.update(nodes="0x0001"))
        self.assertEqual(paths(plan), ["nodes"])


class GroupTest(unittest.TestCase):
    def test_range_edges(self):
        for bad in ("0xEFFF", "0xFFFE", "0xFFFF", "0x1F000"):
            plan = changed(lambda p, b=bad: p["groups"][0].update(address=b))
            self.assertEqual(paths(plan), ["groups[0].address"], bad)

    def test_ip_must_be_multicast(self):
        plan = changed(lambda p: p["groups"][0].update(ip="192.168.1.1"))
        self.assertEqual(paths(plan), ["groups[0].ip"])

    def test_ip_must_parse(self):
        for bad in ("239.1.1", "hello", "", None):
            plan = changed(lambda p, b=bad: p["groups"][0].update(ip=b))
            self.assertEqual(paths(plan), ["groups[0].ip"], bad)

    def test_duplicates(self):
        plan = changed(lambda p: p["groups"][1].update(address="0xF000"))
        self.assertEqual(paths(plan), ["groups[1].address"])
        plan = changed(lambda p: p["groups"][1].update(ip="239.1.1.1"))
        self.assertEqual(paths(plan), ["groups[1].ip"])


class RfHeadTest(unittest.TestCase):
    def test_eui64_forms_are_accepted(self):
        for ok in ("00-11-22-33-44-55-66-77", "0011223344556677",
                   "aa:bb:cc:dd:ee:ff:00:11"):
            plan = changed(lambda p, v=ok: p["rf_heads"][0].update(eui64=v))
            self.assertEqual(netplan.validate(plan), [], ok)

    def test_eui64_bad_forms(self):
        for bad in ("00:11:22:33:44:55:66", "00:11:22:33:44:55:66:77:88",
                    "zz:11:22:33:44:55:66:77", "", None, 5):
            plan = changed(lambda p, v=bad: p["rf_heads"][0].update(eui64=v))
            self.assertEqual(paths(plan), ["rf_heads[0].eui64"], bad)

    def test_eui64_duplicate_ignores_separators_and_case(self):
        plan = changed(lambda p: p["rf_heads"][1].update(
            eui64="00-11-22-33-44-55-66-77".lower()))
        self.assertEqual(paths(plan), ["rf_heads[1].eui64"])

    def test_head_name_rules(self):
        for bad in ("", "has space", "x" * 33, None):
            plan = changed(lambda p, v=bad: p["rf_heads"][0].update(name=v))
            self.assertIn("rf_heads[0].name", paths(plan), bad)

    def test_duplicate_head_name(self):
        plan = changed(lambda p: p["rf_heads"][1].update(name="head0"))
        self.assertIn("rf_heads[1].name", paths(plan))


class NetTest(unittest.TestCase):
    def test_unknown_rf_head(self):
        plan = changed(lambda p: p["nets"][0].update(rf_head="head9"))
        self.assertEqual(paths(plan), ["nets[0].rf_head"])

    def test_second_net_id_for_same_net_and_head(self):
        plan = changed(lambda p: p["nets"].append(
            {"net": "alpha", "rf_head": "head0", "net_id": 99}))
        self.assertEqual(paths(plan), ["nets[2]"])

    def test_same_net_id_on_two_heads_is_allowed(self):
        plan = changed(lambda p: p["nets"][1].update(net_id="0x10"))
        self.assertEqual(netplan.validate(plan), [])

    def test_net_id_must_be_a_non_negative_number(self):
        for bad in ("x", -1, "", None, "1.5"):
            plan = changed(lambda p, v=bad: p["nets"][0].update(net_id=v))
            self.assertEqual(paths(plan), ["nets[0].net_id"], bad)

    def test_net_with_dangling_head_does_not_hide_other_errors(self):
        plan = changed(lambda p: (p["nets"][0].update(rf_head="nope"),
                                  p["nodes"][0].update(address="0x0")))
        self.assertEqual(sorted(paths(plan)), ["nets[0].rf_head", "nodes[0].address"])


class DatatermArgsTest(unittest.TestCase):
    def plan(self):
        plan = copy.deepcopy(GOOD)
        plan["nodes"][0]["ip"] = "10.0.0.1"
        plan["nodes"][2]["ip"] = "10.0.0.16:6000"
        return plan

    def test_args_use_node_and_other_peers_with_ip(self):
        self.assertEqual(netplan.dataterm_args(self.plan(), 1),
                         ["-n", "0x0001", "-P", "0x0010=10.0.0.16:6000"])
        self.assertEqual(netplan.dataterm_args(self.plan(), 0x10),
                         ["-n", "0x0010", "-P", "0x0001=10.0.0.1"])

    def test_bad_ip(self):
        for bad in ("10.0.0", "10.0.0.1:0", "10.0.0.1:99999", "::1", 5):
            plan = self.plan()
            plan["nodes"][0]["ip"] = bad
            self.assertEqual(paths(plan), ["nodes[0].ip"], bad)

    def test_unknown_node_and_invalid_plan_raise(self):
        with self.assertRaises(ValueError):
            netplan.dataterm_args(self.plan(), 7)
        bad = self.plan()
        bad["nodes"][0]["address"] = 0
        with self.assertRaises(ValueError):
            netplan.dataterm_args(bad, 1)


class FileTest(unittest.TestCase):
    def test_round_trip_and_refuse_invalid(self):
        import os
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "plan.json")
            netplan.save_plan(path, GOOD)
            self.assertEqual(netplan.load_plan(path), GOOD)
            bad = copy.deepcopy(GOOD)
            bad["nodes"][0]["address"] = 0
            with self.assertRaises(ValueError):
                netplan.save_plan(os.path.join(tmp, "bad.json"), bad)
            self.assertFalse(os.path.exists(os.path.join(tmp, "bad.json")))


class ParseTest(unittest.TestCase):
    def test_parse_int(self):
        self.assertEqual(netplan.parse_int("0x1F"), 31)
        self.assertEqual(netplan.parse_int(" 12 "), 12)
        self.assertEqual(netplan.parse_int(7), 7)
        for bad in ("0x", "1e3", "-5", "1_0", True, None, 2.0):
            self.assertIsNone(netplan.parse_int(bad), bad)

    def test_normalise_eui64(self):
        self.assertEqual(netplan.normalise_eui64("aa-bb-cc-dd-ee-ff-00-11"),
                         "AABBCCDDEEFF0011")
        self.assertIsNone(netplan.normalise_eui64("aabb"))


if __name__ == "__main__":
    unittest.main()
