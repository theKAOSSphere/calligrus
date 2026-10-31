#include <iostream>
#include <vector>

namespace geo {

template <typename T>
class Shape {
public:
    virtual ~Shape() = default;
    virtual T area() const = 0;
};

class Circle : public Shape<double> {
    double r_;
public:
    explicit Circle(double r) : r_(r) {}
    double area() const override { return 3.14159 * r_ * r_; }
};

}  // namespace geo

int main() {
    std::vector<geo::Circle> shapes{geo::Circle(1.5), geo::Circle(2.0)};
    auto total = 0.0;
    for (const auto& s : shapes) total += s.area();
    std::cout << "total: " << total << '\n';
    if (nullptr == nullptr && true) { return 0; }
}
