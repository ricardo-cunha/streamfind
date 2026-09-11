#include "streamfind/mass_spec/nta.hpp"

#include <cmath>
#include <limits>

namespace nta::utils {
std::ofstream debug_log;
void init_debug_log(const std::string &name, const std::string &header) { debug_log.open(name, std::ios::trunc); if (debug_log) debug_log << header << '\n'; }
void close_debug_log() { if (debug_log.is_open()) debug_log.close(); }
float mean(const std::vector<float> &v) { return v.empty() ? 0.f : std::accumulate(v.begin(), v.end(), 0.f) / v.size(); }
float standard_deviation(const std::vector<float> &v, float m) { if (v.empty()) return 0.f; float s = 0; for (float x : v) s += (x - m) * (x - m); return std::sqrt(s / v.size()); }
float quantile(std::vector<float> v, float q) { if (v.empty()) return 0; q = std::clamp(q, 0.f, 1.f); auto i = static_cast<size_t>((v.size() - 1) * q); std::nth_element(v.begin(), v.begin() + i, v.end()); return v[i]; }
std::string encode_floats_base64(const std::vector<float> &input, int precision) {
    return ::mass_spec::reader::utils::encode_base64(::mass_spec::reader::utils::encode_little_endian_from_float(input, precision));
}
std::vector<size_t> get_sort_indices_float(const std::vector<float> &v) { std::vector<size_t> i(v.size()); std::iota(i.begin(), i.end(), 0); std::sort(i.begin(), i.end(), [&](auto a, auto b) { return v[a] < v[b]; }); return i; }
static void reorder(std::vector<float> &v, const std::vector<size_t> &i) { std::vector<float> out; out.reserve(i.size()); for (auto x : i) out.push_back(v[x]); v = std::move(out); }
static void reorder(std::vector<int> &v, const std::vector<size_t> &i) { std::vector<int> out; out.reserve(i.size()); for (auto x : i) out.push_back(v[x]); v = std::move(out); }
void reorder_multiple_vectors(const std::vector<size_t> &i, std::vector<float> &a, std::vector<float> &b, std::vector<float> &c) { reorder(a,i); reorder(b,i); reorder(c,i); }
void reorder_multiple_vectors(const std::vector<size_t> &i, std::vector<float> &a, std::vector<float> &b, std::vector<float> &c, std::vector<float> &d) { reorder(a,i); reorder(b,i); reorder(c,i); reorder(d,i); }
void reorder_multiple_vectors(const std::vector<size_t> &i, std::vector<float> &a, std::vector<float> &b, std::vector<float> &c, std::vector<float> &d, std::vector<int> &e) { reorder(a,i); reorder(b,i); reorder(c,i); reorder(d,i); reorder(e,i); }
std::vector<size_t> filter_above_threshold(const std::vector<float> &v, const std::vector<float> &t) { std::vector<size_t> out; for (size_t i=0; i<std::min(v.size(),t.size()); ++i) if (v[i] > t[i]) out.push_back(i); return out; }
std::vector<int> cluster_by_threshold_float(const std::vector<float> &v, const std::vector<float> &t) { std::vector<int> out(v.size()); for (size_t i=1; i<v.size(); ++i) out[i] = out[i-1] + (v[i]-v[i-1] > t[std::min(i,t.size()-1)]); return out; }
std::vector<float> calculate_baseline(const std::vector<float> &v, int w) { std::vector<float> out(v.size()); for (size_t i=0;i<v.size();++i) { auto a=i>static_cast<size_t>(w)?i-w:0, b=std::min(v.size()-1,i+static_cast<size_t>(w)); out[i]=*std::min_element(v.begin()+a,v.begin()+b+1); } if(v.size()>2) for(size_t i=1;i+1<v.size();++i) out[i]=(out[i-1]+out[i]+out[i+1])/3; return out; }
std::vector<float> smooth_intensity_savitzky_golay(const std::vector<float> &v, int window, int) { std::vector<float> out(v.size()); auto half=static_cast<size_t>(window/2); for(size_t i=0;i<v.size();++i) { auto a=i>half?i-half:0,b=std::min(v.size()-1,i+half); float s=0; for(auto j=a;j<=b;++j)s+=v[j]; out[i]=s/(b-a+1); } return out; }
void calculate_derivatives(const std::vector<float> &v, std::vector<float> &d1, std::vector<float> &d2) { d1.clear(); d2.clear(); for(size_t i=0;i+1<v.size();++i)d1.push_back(v[i+1]-v[i]); for(size_t i=0;i+1<d1.size();++i)d2.push_back(d1[i+1]-d1[i]); }
float gaussian_function_with_baseline(float A,float mu,float sigma,float base,float x) { return base + A*std::exp(-(x-mu)*(x-mu)/(2*sigma*sigma)); }
void fit_gaussian(const std::vector<float> &x,const std::vector<float> &y,float &A,float &mu,float &sigma,float &base) {
    const float alpha=.01f,beta1=.9f,beta2=.999f,epsilon=1e-8f; float mA=0,vA=0,mMu=0,vMu=0,mS=0,vS=0,mB=0,vB=0;
    for(int iter=1;iter<=500;++iter){float gA=0,gMu=0,gS=0,gB=0; for(size_t i=0;i<x.size();++i){float e=std::exp(-(x[i]-mu)*(x[i]-mu)/(2*sigma*sigma)), err=y[i]-(base+A*e); gA+=-2*err*e; gMu+=-2*err*A*e*(x[i]-mu)/(sigma*sigma); gS+=-2*err*A*e*(x[i]-mu)*(x[i]-mu)/(sigma*sigma*sigma); gB+=-2*err;}
        auto update=[&](float g,float &m,float &v,float &p){m=beta1*m+(1-beta1)*g;v=beta2*v+(1-beta2)*g*g;float mh=m/(1-std::pow(beta1,iter)),vh=v/(1-std::pow(beta2,iter));p-=alpha*mh/(std::sqrt(vh)+epsilon);};
        update(gA,mA,vA,A); A=std::max(.1f,A); update(gMu,mMu,vMu,mu); update(gS,mS,vS,sigma); sigma=std::clamp(sigma,.1f,100.f); update(gB,mB,vB,base); base=std::max(0.f,base);
    }
}
float calculate_gaussian_rsquared(const std::vector<float> &x,const std::vector<float> &y,float A,float mu,float sigma,float base) { if(y.empty())return 0; auto m=mean(y); float total=0,res=0; for(size_t i=0;i<y.size();++i){auto p=gaussian_function_with_baseline(A,mu,sigma,base,x[i]);res+=(y[i]-p)*(y[i]-p);total+=(y[i]-m)*(y[i]-m);} return total?1-res/total:0; }
float calculate_area(const std::vector<float> &x,const std::vector<float> &y) { float a=0; for(size_t i=1;i<x.size()&&i<y.size();++i)a+=(x[i]-x[i-1])*(y[i]+y[i-1])/2; return std::max(0.f,a); }
float calculate_jaggedness(const std::vector<float> &v) { if(v.size()<3)return 0; auto m=*std::max_element(v.begin(),v.end()); if(!m)return 0; float a=0; for(size_t i=1;i+1<v.size();++i)a+=std::abs(v[i]-(v[i-1]+v[i+1])/2); return a/((v.size()-2)*m); }
float calculate_sharpness(const std::vector<float> &x,const std::vector<float> &y,float area) { if(x.empty()||!area)return 0; return *std::max_element(y.begin(),y.end())/((x.back()-x.front())*std::sqrt(std::abs(area))); }
float calculate_asymmetry(const std::vector<float> &x,const std::vector<float> &y) { if(x.size()<3)return 1; auto i=std::distance(y.begin(),std::max_element(y.begin(),y.end())); auto base=std::min(y.front(),y.back()), level=base+(*std::max_element(y.begin(),y.end())-base)*.1f; size_t l=0,r=y.size()-1; for(size_t j=i;j>0;--j)if(y[j]<=level){l=j;break;} for(size_t j=i;j<y.size();++j)if(y[j]<=level){r=j;break;} return l>=i||r<=i?1:(x[r]-x[i])/(x[i]-x[l]); }
int calculate_modality(const std::vector<float> &v,float p) { if(v.size()<3)return 1; auto m=*std::max_element(v.begin(),v.end()); int n=0; for(size_t i=1;i+1<v.size();++i)if(v[i]>v[i-1]&&v[i]>v[i+1]&&v[i]>=m*p)++n; return std::max(1,n); }
float calculate_theoretical_plates(float rt,float width) { return width&&rt?5.54f*std::pow(rt/width,2):0; }
}
