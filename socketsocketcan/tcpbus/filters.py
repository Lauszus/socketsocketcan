from typing import Dict, List, Optional, Union

from can.interfaces.socketcan.constants import CAN_EFF_FLAG

# The maximum number of filters on a CAN socket. See CAN_RAW_FILTER_MAX in linux/can/raw.h
CAN_RAW_FILTER_MAX = 512


def pack_filters(can_filters: Optional[List[Dict[str, Union[int, bool]]]]) -> List[Dict[str, int]]:
    """Convert python-can filters into the IDs and masks used by socketcan. See "pack_filters" in the socketcan
    interface. Like in python-can, no filters means that all frames are received.
    """
    if not can_filters:
        return [{'can_id': 0, 'can_mask': 0}]
    if len(can_filters) > CAN_RAW_FILTER_MAX:
        # The kernel would reject them, so the client would keep using the previous filters
        raise ValueError('At most {} CAN filters are supported, got {}'.format(CAN_RAW_FILTER_MAX, len(can_filters)))

    filter_data = []
    for can_filter in can_filters:
        can_id = can_filter['can_id']
        can_mask = can_filter['can_mask']
        if 'extended' in can_filter:
            # Match on either 11-bit OR 29-bit messages instead of both
            can_mask |= CAN_EFF_FLAG
            if can_filter['extended']:
                can_id |= CAN_EFF_FLAG
        filter_data.append({'can_id': can_id, 'can_mask': can_mask})
    return filter_data
