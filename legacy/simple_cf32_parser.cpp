#include <iostream>
#include <fstream>
#include <vector>
#include <complex>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <cf32_file>" << std::endl;
        return 1;
    }

    std::cout << "🔧 פרסר CF32 פשוט" << std::endl;
    
    // Load samples
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::cerr << "❌ לא ניתן לפתוח: " << argv[1] << std::endl;
        return 1;
    }
    
    std::vector<std::complex<float>> samples;
    float real, imag;
    
    while (file.read(reinterpret_cast<char*>(&real), sizeof(float)) &&
           file.read(reinterpret_cast<char*>(&imag), sizeof(float))) {
        samples.push_back(std::complex<float>(real, imag));
    }
    
    std::cout << "📊 הוטענו " << samples.size() << " דגימות מורכבות" << std::endl;
    
    // בדיקת הספק
    double power = 0;
    for (const auto& s : samples) {
        power += std::norm(s);
    }
    power /= samples.size();
    std::cout << "⚡ הספק ממוצע: " << power << std::endl;
    
    // מציאת פיק
    size_t max_idx = 0;
    float max_power = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        float p = std::norm(samples[i]);
        if (p > max_power) {
            max_power = p;
            max_idx = i;
        }
    }
    std::cout << "🎯 פיק בדגימה " << max_idx << " עם הספק " << max_power << std::endl;
    
    // הצגת דגימות ראשונות
    std::cout << "📝 דגימות ראשונות:" << std::endl;
    for (size_t i = 0; i < std::min((size_t)10, samples.size()); ++i) {
        std::cout << "   [" << i << "] " << samples[i].real() << " + " 
                  << samples[i].imag() << "j" << std::endl;
    }
    
    return 0;
}
