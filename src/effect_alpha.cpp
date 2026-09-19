#include "effect_alpha.hpp"
namespace codm {
int additive_source_factor(const J &parsed,const J &saved) {
    if(!parsed.contains("m_SubShaders") || parsed.at("m_SubShaders").empty())return 0;
    auto value=[&](const J &state){
        auto name=state.value("name",std::string());
        if(!name.empty() && name!="<noninit>") {
            for(const auto &p:saved.at("m_Floats"))if(p.at("first")==name)return p.at("second").get<float>();
            for(const auto &p:parsed.at("m_PropInfo").at("m_Props"))if(p.at("m_Name")==name)return p.value("m_DefValue[0]",0.f);
            return -1.f; // Unknown runtime blend properties are not guessed.
        }
        return state.value("val",0.f);
    };
    for(const auto &pass:parsed.at("m_SubShaders").at(0).at("m_Passes")) {
        if(!pass.contains("m_State") || !pass.at("m_State").contains("rtBlend0"))continue;
        const auto &b=pass.at("m_State").at("rtBlend0");
        if(int(value(b.at("colMask")))%8==0)continue;
        auto src=value(b.at("srcBlend"));
        return value(b.at("destBlend"))==1 && value(b.at("blendOp"))==0 && (src==1 || src==5)?int(src):0;
    }
    return 0;
}
V4 straight_alpha_from_additive(V3 contribution) {
    contribution=glm::clamp(contribution,V3(0),V3(1));
    float alpha=std::max({contribution.x,contribution.y,contribution.z});
    return alpha>0?V4(contribution/alpha,alpha):V4(0);
}
void bleed_transparent_rgb(Image &image) {
    size_t count=size_t(image.width)*image.height;std::vector<uint8_t> visited(count);std::vector<size_t> queue;queue.reserve(count);
    for(size_t i=0;i<count;++i)if(image.pixels[i*4+3]){visited[i]=1;queue.push_back(i);}
    for(size_t q=0;q<queue.size();++q) {
        auto i=queue[q];auto copy=[&](size_t j){if(visited[j])return;visited[j]=1;for(int c=0;c<3;++c)image.pixels[j*4+c]=image.pixels[i*4+c];queue.push_back(j);};
        if(i%image.width)copy(i-1);if(i%image.width+1<size_t(image.width))copy(i+1);
        if(i>=size_t(image.width))copy(i-image.width);if(i+image.width<count)copy(i+image.width);
    }
}
V4 sample_effect_data(const Image &image,V2 uv) {
    uv=glm::fract(uv)*V2(image.width,image.height)-.5f;auto base=glm::floor(uv);auto f=uv-base;
    auto pixel=[&](int x,int y){x=(x%image.width+image.width)%image.width;y=(y%image.height+image.height)%image.height;auto i=(size_t(y)*image.width+x)*4;return V4(image.pixels[i],image.pixels[i+1],image.pixels[i+2],image.pixels[i+3])/255.f;};
    return glm::mix(glm::mix(pixel(int(base.x),int(base.y)),pixel(int(base.x)+1,int(base.y)),f.x),glm::mix(pixel(int(base.x),int(base.y)+1),pixel(int(base.x)+1,int(base.y)+1),f.x),f.y);
}
}
