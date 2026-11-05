import multiprocessing
from typing import Optional, Dict, Union, List

import psutil
# noinspection PyUnresolvedReferences
from tcpclient import tcpclient

from .filters import pack_filters


class TCPClient(object):

    def __init__(self, channel: str, hostname: str, port: int, can_filters: List[Dict[str, Union[int, bool]]] = None,
                 use_unordered_map=False, limit_recv_rate_hz: int = None, ignore_rx_error_frames=False):
        """
        :param channel: Can interface to use.
        :param hostname: Hostname used for the socket.
        :param port: The port to use for the TCP connection.
        :param can_filters: Can ID and mask to apply to all incoming frames.
        :param use_unordered_map: Override the any old frames if a new one is received before the server has had time
        to read it yet. This can be used together with "limit_recv_rate_hz" to limit the update rate while still
        receiving the latest data.
        :param limit_recv_rate_hz: Allow to limit the receive update rate.
        :param ignore_rx_error_frames: Do not receive error frames. This avoids waking up the client for every error
        frame on a busy bus when only a few IDs are of interest.
        """
        self.channel = channel
        self.hostname = hostname
        self.port = port
        self.can_filters = can_filters
        self.use_unordered_map = use_unordered_map
        self.limit_recv_rate_hz = limit_recv_rate_hz
        self.ignore_rx_error_frames = ignore_rx_error_frames

        # Run the client in a separate process, so it does not block the main thread
        self._tcp_client_process = multiprocessing.Process(target=self._tcp_client, args=(channel, hostname, port,
                                                                                          can_filters,
                                                                                          use_unordered_map,
                                                                                          limit_recv_rate_hz,
                                                                                          ignore_rx_error_frames))
        self._tcp_client_process.daemon = True
        self._tcp_client_process.start()

    @property
    def pid(self):
        return self._tcp_client_process.pid

    def is_alive(self):
        self._tcp_client_process.is_alive()

    def join(self, timeout=None):
        self._tcp_client_process.join(timeout=timeout)

    def shutdown(self, timeout=None) -> Optional[int]:
        # Send SIGTERM to the process and wait for it to finish
        if self._tcp_client_process.is_alive():
            self._tcp_client_process.terminate()
            self._tcp_client_process.join(timeout=timeout)

        # The exitcode will be None if the process is still alive
        return self._tcp_client_process.exitcode

    def kill(self, timeout=None):
        # Get an instance of the process
        process = psutil.Process(self.pid)

        # Send SIGKILL to the process
        process.kill()

        # Wait for the process to end
        return process.wait(timeout)

    @staticmethod
    def _tcp_client(channel, hostname, port, can_filters, use_unordered_map, limit_recv_rate_hz,
                    ignore_rx_error_frames):
        tcpclient(channel, hostname, port, pack_filters(can_filters), use_unordered_map, limit_recv_rate_hz,
                  ignore_rx_error_frames)
