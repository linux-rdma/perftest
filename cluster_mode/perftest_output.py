"""
perftest_output.py - Parse parameter blocks printed by perftest workers.
"""

from __future__ import annotations

import re
from typing import Dict, List


CLUSTER_PHASE_PREFIX = "[Cluster] "

PARAM_LINE_RE = re.compile(r'^\s+\S.*:\s+\S')
TEST_NAME_RE = re.compile(r'(BW|Latency|LAT)\s+Test')
SEPARATOR_RE = re.compile(r'^-{20,}$')

PARAM_KEY_MAP = {
    'number of qps':        'qps',
    'tx depth':             'tx_depth',
    'rx depth':             'rx_depth',
    'mtu':                  'mtu',
    'link type':            'link_type',
    'transport type':       'transport_type',
    'connection type':      'connection_type',
    'using srq':            'srq',
    'number of iterations': 'iters',
    'duration':             'duration',
    'post list':            'post_list',
    'cq moderation':        'cq_mod',
    'gid index':            'gid_index',
    'outstand reads':       'outstanding_reads',
}


def parse_perftest_params(params_lines: List[str]) -> Dict[str, str]:
    """Parse the raw perftest parameter block into normalized key/value pairs."""
    result: Dict[str, str] = {}
    chunk_re = re.compile(r'([^:]+?)\s*:\s*(\S+(?:\s+\S+)*?)(?:\s{2,}|$)')
    for line in params_lines:
        for match in chunk_re.finditer(line):
            raw_key = match.group(1).strip().lower()
            raw_val = match.group(2).strip()
            norm_key = PARAM_KEY_MAP.get(raw_key)
            if norm_key and norm_key not in result:
                result[norm_key] = raw_val
    return result
