#include "sketch/georeferencing_contract.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace sketch;
void require(bool ok) { if (!ok) throw std::runtime_error("georeference assertion failed"); }
template<class F> void rejects(F f) { try { f(); } catch (const std::invalid_argument&) { return; } throw std::runtime_error("invalid georeference accepted"); }
int main() {
    try {
        const GeoCrs crs{"EPSG:32613", "fixture projected metre CRS", GeoCoordinateUnit::metre};
        const OfflineGeoResources offline{false, {{"proj/proj.db", std::string(64, 'a')}}};
        const AffineGeoTransform transform{2, 0, 10, 0, 3, 20};
        const std::vector<GeoControlPoint> points{{"b", 1, 0, 12, 20}, {"a", 0, 0, 10, 20}, {"c", 0, 1, 10, 26}};
        const GeoreferencingContract value(crs, transform, points, offline);
        require(value.apply(1, 1).x == 12 && value.apply(1, 1).y == 23);
        require(value.residuals().back().dy == -3 && value.maximum_residual_m() == 3);
        require(std::abs(value.rms_residual_m()-std::sqrt(3.0)) < 1e-12);
        auto reversed = points; std::reverse(reversed.begin(), reversed.end());
        require(value.serialize() == GeoreferencingContract(crs, transform, reversed, offline).serialize());
        require(nlohmann::json::parse(value.serialize())["network_enabled"] == false);
        const auto roundtrip = GeoreferencingContract::from_json(nlohmann::json::parse(value.serialize()));
        require(roundtrip.serialize() == value.serialize());
        auto tampered = nlohmann::json::parse(value.serialize());
        tampered["rms_residual_m"] = 0.0;
        rejects([&] { (void)GeoreferencingContract::from_json(tampered); });
        tampered = nlohmann::json::parse(value.serialize());
        tampered["crs"]["extra"] = true;
        rejects([&] { (void)GeoreferencingContract::from_json(tampered); });
        tampered = nlohmann::json::parse(value.serialize());
        tampered["control_points"][0]["target_easting_m"] = 999.0;
        rejects([&] { (void)GeoreferencingContract::from_json(tampered); });
        rejects([&] { auto t = transform; t.a = 0; (void)GeoreferencingContract(crs, t, points, offline); });
        rejects([&] { auto p = points; p[1].id = "b"; (void)GeoreferencingContract(crs, transform, p, offline); });
        rejects([&] { auto r = offline; r.network_enabled = true; (void)GeoreferencingContract(crs, transform, points, r); });
        rejects([&] { auto r = offline; r.files[0].relative_path = "../proj.db"; (void)GeoreferencingContract(crs, transform, points, r); });
        rejects([&] { auto c = crs; c.unit = static_cast<GeoCoordinateUnit>(99); (void)GeoreferencingContract(c, transform, points, offline); });
        for (const auto* bad : {"/proj.db", "C:/proj.db", "proj\\proj.db", "proj//proj.db", "proj/", "NUL.db", "proj./proj.db", "https://x"}) {
            rejects([&] { auto r=offline; r.files[0].relative_path=bad; (void)GeoreferencingContract(crs,transform,points,r); });
        }
        rejects([&] { auto r=offline; r.files.push_back({"PROJ/PROJ.DB",std::string(64,'b')}); (void)GeoreferencingContract(crs,transform,points,r); });
        rejects([&] { auto r=offline; r.files[0].sha256="bogus"; (void)GeoreferencingContract(crs,transform,points,r); });
        rejects([&] { (void)GeoreferencingContract(crs,transform,points,{}); });
        rejects([&] { (void)GeoreferencingContract(crs,transform,{},offline); });
        rejects([&] { auto t=transform; t.tx=std::numeric_limits<double>::quiet_NaN(); (void)GeoreferencingContract(crs,t,points,offline); });
        rejects([&] { auto t=transform; t.a=1e-20; (void)GeoreferencingContract(crs,t,points,offline); });
        rejects([&] { auto p=points; p[0].target_x=std::numeric_limits<double>::quiet_NaN(); (void)GeoreferencingContract(crs,transform,p,offline); });
        rejects([&] { auto c=crs; c.identifier=std::string(1,static_cast<char>(0xff)); (void)GeoreferencingContract(c,transform,points,offline); });
        rejects([&] { (void)GeoreferencingContract(crs,transform,std::vector<GeoControlPoint>(GeoreferencingContract::maximum_control_points+1),offline); });
        rejects([&] { (void)value.apply(std::numeric_limits<double>::infinity(), 1); });
        rejects([&] { (void)value.apply(1e308, 1e308); });
        std::cout << "georeferencing contract tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
