#include <iostream>
#include <fstream>
#include <vector>
#include <complex>
int main() {
    std::vector<std::complex<float>> samples;
    std::ifstream file("temp/hello_world.cf32", std::ios::binary);
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0);
    samples.resize(size / sizeof(std::complex<float>));
    file.read((char*)samples.data(), size);
    file.close();
    std::cout << "Sample at 11484: (" << samples[11484].real() << "," << samples[11484].imag() << ")" << std::endl;
    return 0;
}
