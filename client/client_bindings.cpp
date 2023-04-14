/* MIT License
 *
 * Copyright (c) 2019 Thomas Bruen
 * Copyright (c) 2019-2023 Kristian Sloth Lauszus
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <iostream>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <linux/can.h>

namespace py = pybind11;

int tcpclient(const char *can_port, const char *hostname, int port, const struct can_filter *filter, int numfilter, bool use_unordered_map, int limit_recv_rate_hz);

int tcpclient_wrapper(const char *can_port, const char *hostname, int port, py::list filters, bool use_unordered_map, py::object limit_recv_rate_hz_obj) {
    int numfilter = filters.size();
    int limit_recv_rate_hz = limit_recv_rate_hz_obj.is_none() ? -1 : limit_recv_rate_hz_obj.cast<int>();
    //py::print(limit_recv_rate_hz);

    int retval = -1;
    if (numfilter == 0)
    {
        retval = tcpclient(can_port, hostname, port, NULL, 0, use_unordered_map, limit_recv_rate_hz);
    }
    else
    {
        struct can_filter *rfilter = (struct can_filter*)malloc(sizeof(struct can_filter) * numfilter);
        int i = 0;
        for (auto filter : filters) {
            //py::print(filter);
            rfilter[i].can_id = filter["can_id"].cast<canid_t>();
            rfilter[i].can_mask = filter["can_mask"].cast<canid_t>();
            i++;
        }
        retval = tcpclient(can_port, hostname, port, rfilter, numfilter, use_unordered_map, limit_recv_rate_hz);
        free(rfilter);
    }
    return retval;
}

PYBIND11_MODULE(tcpclient, m) {
    m.def("tcpclient", &tcpclient_wrapper);
}
