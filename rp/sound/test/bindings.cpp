// #include <pybind11/pybind11.h>
// #include <pybind11/stl.h>       // needed for automatic vector conversion
// #include "../frame_scanner.hpp"
//
// namespace py = pybind11;
//
// PYBIND11_MODULE(frame_scanner, m) {
//     py::class_<FrameData>(m, "FrameData")
//         .def_readonly("rawBits", &FrameData::rawBits);
//
//     py::class_<Mp3FrameScanner>(m, "Mp3FrameScanner")
//         .def(py::init<const std::string &>())
//         .def_readonly("frame_data", &Mp3FrameScanner::frame_data);
// }