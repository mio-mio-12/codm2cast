#include "gallery.hpp"
#include <regex>
namespace codm {
std::string weapon_icon_key(std::string name) {
    static const std::regex prefix("^(mainweapon|assiweapon|secondaryweapon|throwweapon)_[0-9]+_");
    static const std::regex pov("^pov_");
    static const std::regex perspective("_(1p|3p|pov|ui)(?:_lod[0-9]+)?$");
    static const std::regex punctuation("[^a-z0-9]");
    name = lower(name);
    name = std::regex_replace(name, prefix, "");
    name = std::regex_replace(name, pov, "");
    name = std::regex_replace(name, perspective, "");
    name = std::regex_replace(name, punctuation, "");
    return name;
}
J weapon_icon_index(Source &source, const fs::path &cache, JobContext *job) {
    auto started = Clock::now();
    J bundles = J::array();
    for (const auto &b : source.catalog.at("bundles")) {
        auto name = hint(b.at("path"));
        if (name.find("textures/ui_icon/icon/weapon/") != std::string::npos ||
            name.find("textures/ui_icon/hall/hall_draw/weapon/") != std::string::npos ||
            name.find("textures/ui_icon/icon/baseweapon.pak") != std::string::npos)
            bundles.push_back(b);
    }
    auto signature = hex64(hash64(bundles.dump() + "gallery4"));
    auto indexPath = cache / (signature + ".json");
    if (fs::exists(indexPath)) {
        auto result = read_json(indexPath);
        result["cacheHit"] = true;
        result["seconds"] = seconds(started);
        return result;
    }
    J icons = J::object(), errors = J::array();
    size_t done = 0;
    for (const auto &b : bundles) {
        if (job) job->update(float(done++) / std::max(size_t(1),bundles.size()), "Indexing weapon icons");
        source.clear();
        try {
            source.load(b.at("path"));
            for (const auto &n : b.at("nodes")) {
                if (!(n.at("flags").get<int>() & 4)) continue;
                auto &file = source.file(n.at("name"));
                for (auto &[pid,o] : file.objects) {
                    if (o.cid != 114) continue;
                    Reader r(o.raw(), file.big);
                    r.seek(16);
                    auto pointer = [&]() { auto f=r.get<int32_t>(); auto p=r.get<int64_t>();
                        return J{{"m_FileID",f},{"m_PathID",p}}; };
                    auto *script = source.ref(o,pointer());
                    if (!script || source.tree(*script).value("m_ClassName",std::string()) != "UIAtlas") continue;
                    r.str();
                    auto *material = source.ref(o,pointer());
                    if (!material || material->cid != 21) continue;
                    Object *texture = nullptr;
                    for (const auto &p : source.tree(*material).at("m_SavedProperties").at("m_TexEnvs"))
                        if (p.at("first") == "_MainTex") texture = source.ref(*material,p.at("second").at("m_Texture"));
                    if (!texture || texture->cid != 28) continue;
                    const auto &tt = source.tree(*texture);
                    int tw=tt.at("m_Width"), th=tt.at("m_Height");
                    int count=r.count(52,20000);
                    for (int i=0;i<count;++i) {
                        auto name=r.str(); name.erase(std::find(name.begin(),name.end(),'\0'),name.end());
                        int x=r.get<int>(), y=r.get<int>(), w=r.get<int>(), h=r.get<int>();
                        r.take(8*4); // NGUI border and padding fields.
                        require(x>=0 && y>=0 && w>0 && h>0 && w<=tw-x && h<=th-y, "Invalid icon atlas rectangle");
                        auto key=weapon_icon_key(name);
                        if (key.empty()) continue;
                        J icon={{"name",name},{"texture",texture->id()},{"x",x},{"y",y},{"width",w},{"height",h},
                                {"bundle",b.at("path")},{"signature",signature}};
                        // Prefer the largest authored icon for an exact variant name.
                        if (!icons.contains(key) || w*h > icons[key].at("width").get<int>()*icons[key].at("height").get<int>())
                            icons[key]=icon;
                    }
                }
            }
        } catch (const std::exception &e) { errors.push_back({{"bundle",b.at("path")},{"error",e.what()}}); }
    }
    if(job) job->check();
    J result={{"icons",icons},{"errors",errors},{"bundles",bundles.size()},
              {"cacheHit",false},{"seconds",seconds(started)}};
    write_json(indexPath,result);
    return result;
}
Image weapon_thumbnail(const J &icon, const fs::path &cache, const fs::path &catalog,
                       const fs::path &data, std::unique_ptr<Source> &source,
                       std::map<std::string, Image> &atlases) {
    auto path=cache/(hex64(hash64(icon.dump()+"thumb1"))+".rgba");
    constexpr int width=192,height=96;
    if(fs::exists(path)) {
        auto bytes=read_bytes(path);
        if(bytes.size()==width*height*4) return {width,height,std::move(bytes)};
    }
    if(!source) source=std::make_unique<Source>(catalog,data);
    std::string id=icon.at("texture");
    if(!atlases.contains(id)) {
        // At most two source atlases per batch; cached thumbnails are tiny.
        if(atlases.size()>=2) { atlases.clear(); source->clear(); }
        auto &object=source->object(id);
        const auto &t=source->tree(object);
        require(t.at("m_Width").get<int>()<=8192 && t.at("m_Height").get<int>()<=8192,"Icon atlas too large");
        atlases[id]=decode_texture(*source,object);
    }
    const auto &atlas=atlases.at(id);
    int x=icon.at("x"),y=icon.at("y"),w=icon.at("width"),h=icon.at("height");
    require(x>=0 && y>=0 && w>0 && h>0 && w<=atlas.width-x && h<=atlas.height-y,"Invalid cached icon rectangle");
    Image crop{w,h,Bytes(size_t(w)*h*4)};
    for(int row=0;row<h;++row)
        std::copy_n(atlas.pixels.begin()+(size_t(y+row)*atlas.width+x)*4,w*4,crop.pixels.begin()+size_t(row)*w*4);
    double scale=std::min(double(width)/w,double(height)/h);
    auto small=resize_image(crop,std::max(1,int(w*scale)),std::max(1,int(h*scale)));
    Image out{width,height,Bytes(width*height*4,0)};
    int dx=(width-small.width)/2,dy=(height-small.height)/2;
    for(int row=0;row<small.height;++row)
        std::copy_n(small.pixels.begin()+size_t(row)*small.width*4,small.width*4,
                    out.pixels.begin()+(size_t(row+dy)*width+dx)*4);
    fs::create_directories(cache);
    std::ofstream f(path,std::ios::binary); f.write(reinterpret_cast<const char*>(out.pixels.data()),out.pixels.size());
    return out;
}
}
