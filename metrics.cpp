#include "metrics.h"

#include <cmath>
#include <stdexcept>

double calcMSE(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height || a.channels != b.channels) {
        throw std::runtime_error("calcMSE: image sizes do not match");
    }

    if (a.data.empty() || b.data.empty() || a.data.size() != b.data.size()) {
        throw std::runtime_error("calcMSE: invalid image data");
    }

    double sum = 0.0;

    for (std::size_t i = 0; i < a.data.size(); ++i) {
        const double diff =
            static_cast<double>(a.data[i]) - static_cast<double>(b.data[i]);
        sum += diff * diff;
    }

    return sum / static_cast<double>(a.data.size());
}

double calcPSNR(const Image& a, const Image& b) {
    const double mse = calcMSE(a, b);

    if (mse <= 0.0) {
        return 99.0;
    }

    const double maxValue = 255.0;

    return 10.0 * std::log10((maxValue * maxValue) / mse);
}