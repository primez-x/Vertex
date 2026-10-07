#include "support/noninteractive_errors.hpp"
#include "sketch/svg_admission.hpp"
#include "sketch/desktop/svg_admission.hpp"
#include "sketch/annotation_catalog.hpp"
#include "sketch/desktop/symbol_svg_palette.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "unsafe or malformed SVG was admitted");
}
QByteArray nested(const char* element, int depth) {
    QByteArray result("<svg xmlns='http://www.w3.org/2000/svg'>");
    for (int i = 1; i < depth; ++i) result += QByteArray("<") + element + ">";
    for (int i = 1; i < depth; ++i) result += QByteArray("</") + element + ">";
    return result + "</svg>";
}
// These assertions use only the headless admission API. In particular, the
// exponential input is never passed to native construction or drawing.
std::string marker_chain(int levels,int fanout,bool css=false,bool subpaths=false) {
    std::string source="<svg xmlns='http://www.w3.org/2000/svg'><defs>";
    if (css) {
        source+="<style><![CDATA[";
        for (int level=0;level+1<levels;++level)
            source+=".c"+std::to_string(level)+" {marker-end: \\75rl(#m"+std::to_string(level+1)+");}";
        source+="]]></style>";
    }
    for (int level=0;level<levels;++level) {
        source+="<marker id='m"+std::to_string(level)+"'>";
        if (level+1==levels) source+="<circle r='1'/>";
        else {
            const int paths=subpaths ? 1 : fanout;
            for (int path=0;path<paths;++path) {
                source+="<path d='";
                for (int subpath=0;subpath<(subpaths ? fanout : 1);++subpath) source+="M0 0L1 1 ";
                source+="' ";
                source+=css ? "class='c"+std::to_string(level)+"'" : "marker-end='url(#m"+std::to_string(level+1)+")'";
                source+="/>";
            }
        }
        source+="</marker>";
    }
    return source+="</defs><path d='M0 0L1 1' marker-end='url(#m0)'/></svg>";
}
void replace_all(std::string& source,std::string_view from,std::string_view to) {
    for (std::size_t pos=0;(pos=source.find(from,pos))!=std::string::npos;pos+=to.size())
        source.replace(pos,from.size(),to);
}
void case_distinct_attributes(std::string& source,std::string_view attribute,std::string_view ignored) {
    const auto prefix=std::string(attribute)+"='";
    for (std::size_t pos=0;(pos=source.find(prefix,pos))!=std::string::npos;) {
        const auto begin=pos+prefix.size(),end=source.find('\'',begin);
        require(end!=std::string::npos,"case-distinct attribute fixture is malformed");
        const auto extra=" "+std::string(ignored)+"='decoy"+source.substr(begin,end-begin)+"'";
        source.insert(end+1,extra);
        pos=end+1+extra.size();
    }
}
std::string styled_polyline_chain(std::string_view property_prefix) {
    auto source=marker_chain(12,1);
    std::string points;
    for (int point=0;point<18;++point) points+=std::to_string(point)+",0 ";
    replace_all(source,"<path d='M0 0L1 1 '","<polyline points='"+points+"'");
    for (int level=1;level<12;++level) {
        const auto fragment="url(#m"+std::to_string(level)+")";
        replace_all(source,"marker-end='"+fragment+"'","style='"+std::string(property_prefix)+
            "marker-start:"+fragment+";"+std::string(property_prefix)+"marker-mid:"+fragment+"'");
    }
    return source;
}
void resource_expansion_regressions() {
    // Near the finite policy boundary: the independent arithmetic oracle gives
    // 61,396 versus 122,833 conservative visits for these compact inputs.
    sketch::validate_svg_structure(marker_chain(13,2));
    rejects([&] { sketch::validate_svg_structure(marker_chain(14,2)); });
    auto xml_ids=marker_chain(12,16);
    for (std::size_t pos=0;(pos=xml_ids.find("id='",pos))!=std::string::npos;pos+=8)
        xml_ids.replace(pos,4,"xml:id='");
    rejects([&] {sketch::validate_svg_structure(xml_ids);});
    auto normalized_ids=marker_chain(12,16);
    for (std::size_t pos=0;(pos=normalized_ids.find("id='m",pos))!=std::string::npos;pos+=6)
        normalized_ids.replace(pos,5,"id='m\n");
    for (std::size_t pos=0;(pos=normalized_ids.find("url(#m",pos))!=std::string::npos;pos+=7)
        normalized_ids.replace(pos,6,"url(#m ");
    rejects([&] {sketch::validate_svg_structure(normalized_ids);});
    auto case_ids=marker_chain(12,16);
    case_distinct_attributes(case_ids,"id","ID");
    rejects([&] {sketch::validate_svg_structure(case_ids);});
    auto case_xml_ids=marker_chain(12,16);
    replace_all(case_xml_ids,"id='","xml:id='");
    case_distinct_attributes(case_xml_ids,"xml:id","xml:ID");
    rejects([&] {sketch::validate_svg_structure(case_xml_ids);});
    auto case_classes=marker_chain(12,16,true);
    case_distinct_attributes(case_classes,"class","CLASS");
    rejects([&] {sketch::validate_svg_structure(case_classes);});
    auto case_types=marker_chain(12,16,true);
    replace_all(case_types,".c","PATH.c");
    rejects([&] {sketch::validate_svg_structure(case_types);});
    auto escaped_ids=marker_chain(12,16);
    for (int level=11;level>=0;--level) {
        const auto id="m"+std::to_string(level);
        replace_all(escaped_ids,"id='"+id+"'","id='"+id+"\\31'");
        replace_all(escaped_ids,"url(#"+id+")","url(#"+id+"\\31)");
    }
    rejects([&] {sketch::validate_svg_structure(escaped_ids);});
    auto unicode_urls=marker_chain(12,16);
    replace_all(unicode_urls,")'","&#160;)' ");
    rejects([&] {sketch::validate_svg_structure(unicode_urls);});
    auto unicode_after_keyword=marker_chain(12,16);
    replace_all(unicode_after_keyword,"url(","url&#160;(");
    rejects([&] {sketch::validate_svg_structure(unicode_after_keyword);});
    auto bare_urls=marker_chain(12,16);
    replace_all(bare_urls,"url(","(");
    rejects([&] {sketch::validate_svg_structure(bare_urls);});
    // Native CSS whitespace/comments must not turn marker multiplicity into
    // a generic weight-one resource edge. Keep these chains headless only.
    rejects([&] {sketch::validate_svg_structure(styled_polyline_chain("\\c "));});
    rejects([&] {sketch::validate_svg_structure(styled_polyline_chain("\\2f* x */"));});
    for (bool css:{false,true}) {
        sketch::validate_svg_structure(marker_chain(12,1,css));
        sketch::validate_svg_structure(marker_chain(5,4,css));
        const auto fanout=marker_chain(12,16,css);
        require(fanout.size()<sketch::svg_document_byte_limit,"fanout fixture must stay within the existing byte limit");
        rejects([&] { sketch::validate_svg_structure(fanout); });
    }
    // Conservatively account for multiple subpaths rather than depending on
    // the native renderer invoking an endpoint just once per XML path node.
    rejects([&] { sketch::validate_svg_structure(marker_chain(12,16,false,true)); });
    for (const auto* source:{
        "<svg><defs><marker id='m'><path marker-end='url(#m)'/></marker></defs></svg>",
        "<svg><defs><pattern id='a'><rect fill='url(#b)'/></pattern><pattern id='b'><rect fill='url(#a)'/></pattern></defs></svg>",
        "<svg><style>.a:not(.b) {fill:url(#paint)}</style><defs><linearGradient id='paint'/></defs><rect class='a'/></svg>",
        "<svg><style>path{fill: \\75rl(https://example.test/x)}</style><path/></svg>",
        "<svg><path style='fill:u\\72l (file:///x)'/></svg>",
        "<svg><path fill='url(#missing)'/></svg>",
        "<svg><style>path/* ambiguous tokenizer boundary */.a {marker-end:url(#m)}</style><marker id='m'><path/></marker></svg>",
        "<svg><defs><marker id='m'/><marker id='m'/></defs></svg>",
        "<svg><style><![CDATA[path{marker-end:url(#m)}]]></style><marker id='m'><path/></marker></svg>"})
        rejects([&] { sketch::validate_svg_structure(source); });
    // Numeric references and CSS escapes resolve before graph lookup. Keep
    // benign gradients, masks, patterns, inheritance and quoted URLs intact.
    sketch::validate_svg_structure("<svg><defs><marker id='m&#49;'><path/></marker><linearGradient id='paint'/><pattern id='p'><rect fill='url(#paint)'/></pattern><mask id='mask'><rect fill='white'/></mask></defs><style>g > path.a {marker-end:url(\"#m1\")}</style><g fill='url(#p)' mask='url(#mask)'><path class='a' d='M0 0L1 1'/></g></svg>");
    // Distinct XML names, raw presentation IDs, Unicode Qt whitespace and
    // case-insensitive CSS type selectors remain usable for bounded artwork.
    sketch::validate_svg_structure("<svg><defs><marker id='m\\31' ID='ignored'><path/></marker></defs><path d='M0 0L1 1' marker-end='url(#m\\31&#160;)'/></svg>");
    sketch::validate_svg_structure("<svg><defs><marker xml:id='m' xml:ID='ignored'><path/></marker></defs><style>PATH.a {marker-end:url(#m)}</style><path class='a' CLASS='ignored' d='M0 0L1 1'/></svg>");
    // Source text remains pinned authority; admission does not rewrite it.
    auto pinned=marker_chain(5,4,true);const auto retained=pinned;
    sketch::validate_svg_structure(pinned);
    require(pinned==retained,"resource admission changed pinned artwork bytes");
}
void optional_guide_regressions() {
    const QByteArray prefixed="<s:svg xmlns:s='http://www.w3.org/2000/svg'><s:rect width='10' height='10'/></s:svg>";
    sketch::desktop::validate_svg_document(prefixed);
    QSvgRenderer native;
    require(sketch::desktop::load_admitted_svg(native,prefixed),"bounded namespace-prefixed SVG was refused");
    for (const auto& source:{nested("marker",65)}) {
        for (bool selected:{false,true}) {
            sketch::desktop::PlanCanvas canvas;
            canvas.setAttribute(Qt::WA_DontShowOnScreen);canvas.resize(100,100);
            sketch::desktop::CanvasEntity entity;
            entity.id=QStringLiteral("optional-guide-invalid-svg");entity.type=QStringLiteral("symbol");entity.selected=selected;
            sketch::desktop::CanvasSvgSymbol artwork;
            artwork.document=source;artwork.view_box=artwork.footprint_view_box=QRectF(0,0,10,10);
            artwork.width_metres=artwork.depth_metres=1;entity.svg_symbol=artwork;
            canvas.setGridEnabled(false);canvas.setEntities({entity});
            canvas.setSketchCompositionGuideEnabled(true);
            QImage screen(100,100,QImage::Format_ARGB32_Premultiplied);screen.fill(Qt::transparent);
            canvas.render(&screen); // Actual QWidget paintEvent must not throw.
            require(!canvas.sketchCompositionGuideRect(),"invalid SVG produced an optional composition frame");
            QPainter output(&screen);
            rejects([&] {canvas.renderSceneAt(output,QRectF(screen.rect()),10,{0,0},Qt::transparent);});
            output.end();
            // The recorder is also an explicit output API and remains strict.
            rejects([&] {(void)canvas.recordSketchContent();});
        }
    }
}
void bounded_native_resource_semantics() {
    const QByteArray control="<svg xmlns='http://www.w3.org/2000/svg' width='40' height='20' viewBox='0 0 40 20'><defs><marker id='m' markerUnits='userSpaceOnUse' markerWidth='8' markerHeight='8' refX='0' refY='0' viewBox='0 0 8 8'><rect width='6' height='6' fill='red'/></marker></defs><path d='M4 4L20 4' fill='none' stroke='black' marker-end='url(#m)'/></svg>";
    const auto render=[](const QByteArray& source) {
        QSvgRenderer renderer;
        require(sketch::desktop::load_admitted_svg(renderer,source),"bounded resource semantic fixture refused");
        QImage image(80,40,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);
        QPainter painter(&image);renderer.render(&painter);painter.end();return image;
    };
    const auto expected=render(control);
    int red_pixels=0;
    for (int y=0;y<expected.height();++y) for (int x=0;x<expected.width();++x)
        if (expected.pixelColor(x,y)==QColor(Qt::red)) ++red_pixels;
    require(red_pixels>10,"bounded marker semantic control did not actually render its resource");
    auto distinct=control;distinct.replace("id='m'","id='m' ID='ignored'");
    require(render(distinct)==expected,"case-distinct XML attributes changed native resource selection");
    auto raw_id=control;raw_id.replace("id='m'","id='m\\31'");raw_id.replace("#m)","#m\\31)");
    require(render(raw_id)==expected,"literal presentation URL IDs changed native resource selection");
    auto whitespace=control;whitespace.replace("#m)","#m&#160;)");
    require(render(whitespace)==expected,"Qt Unicode URL trimming changed resource paint");
    auto bare=control;bare.replace("url(#m)","(#m)");
    require(render(bare)==expected,"bare extended-attribute fragment lost native marker paint");
    auto styled=control;styled.replace("<defs>","<style>PATH.a {marker-end:url(#m)}</style><defs>");
    styled.replace("marker-end='url(#m)'","class='a' CLASS='ignored'");
    auto lower_styled=styled;lower_styled.replace("PATH.a", "path.a");
    auto inline_styled=control;inline_styled.replace("marker-end='url(#m)'", "style='marker-end:url(#m)'");
    std::cerr << "bounded CSS marker equivalence: uppercase=" << (render(styled)==expected)
              << " lowercase=" << (render(lower_styled)==expected)
              << " inline=" << (render(inline_styled)==expected) << '\n';
    // Qt's stylesheet name index is case sensitive even though its later
    // type-name matcher is not. Keep admission conservative for uppercase
    // selectors; native equivalence uses the indexable lowercase spelling.
    require(render(lower_styled)==expected,"CSS or XML class-case semantics lost resource paint");
    auto class_only=lower_styled;class_only.replace("path.a", ".a");
    require(render(class_only)==expected,"class-only stylesheet lost native marker paint");
}

}
int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication app(argc, argv);
    try {
        resource_expansion_regressions();
        bounded_native_resource_semantics();
        using sketch::desktop::load_admitted_svg;
        QSvgRenderer::setDefaultOptions(QtSvg::Option::AssumeTrustedSource);
        for (const auto* element : {"g", "marker", "mask", "pattern", "defs"}) {
            sketch::validate_svg_structure(nested(element, 64).toStdString());
            sketch::desktop::validate_svg_document(nested(element, 64));
            const auto excessive = nested(element, 65);
            rejects([&] { sketch::validate_svg_structure(excessive.toStdString()); });
            QSvgRenderer renderer;
            rejects([&] { (void)load_admitted_svg(renderer, excessive); });
            require(!renderer.isValid(), "rejected XML reached QtSvg");
        }
        for (const auto* source : {"<svg><g></svg>", "<svg/><svg/>", "<svg><path d='x></svg>",
             "<svg a='1' a='2'/>", "<svg>&unknown;</svg>", "<svg>&#0;</svg>", "<svg><unbound:g/></svg>",
             "<!DOCTYPE svg [<!ENTITY a 'x'>]><svg>&a;</svg>",
             "<?xml-stylesheet href='https://example.test/x'?><svg/>",
             "<svg><image href='file:///x'/></svg>", "<svg><xi:include xmlns:xi='http://www.w3.org/2001/XInclude'/></svg>",
             "<svg><path fill='url(&#104;ttps://example.test/a)'/></svg>"}) {
            QSvgRenderer renderer;
            rejects([&] { (void)load_admitted_svg(renderer, QByteArray(source)); });
        }
        QByteArray boundary_bytes("<svg>");
        boundary_bytes += QByteArray(262133, ' '); boundary_bytes += "</svg>";
        require(boundary_bytes.size() == 262144, "byte-boundary fixture is wrong");
        sketch::validate_svg_structure(boundary_bytes.toStdString());
        rejects([&] { sketch::validate_svg_structure((boundary_bytes + ' ').toStdString()); });
        const QByteArray ordinary = "<?xml version='1.0'?><svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10'><title>A &amp; B</title><defs><marker id='m'><path d='M0 0L1 1'/></marker><mask id='mask'><rect width='10' height='10' fill='white'/></mask><linearGradient id='paint'><stop offset='0' stop-color='red'/><stop offset='1' stop-color='red'/></linearGradient></defs><g mask='url(#mask)'><rect width='10' height='10' fill='url(#paint)'/></g></svg>";
        QSvgRenderer renderer;
        require(load_admitted_svg(renderer, ordinary), "ordinary marker/mask/internal reference SVG refused");
        require(renderer.options() == QtSvg::Option::NoOption, "ambient SVG options survived explicit admission");
        QImage image(10, 10, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
        QPainter painter(&image); renderer.render(&painter); painter.end();
        require(image.pixelColor(5, 5) == QColor(Qt::red), "admission altered ordinary SVG geometry/paint");
        // A caller that bypasses document projection must still refuse output,
        // rather than quietly export the retained fallback footprint.
        sketch::desktop::PlanCanvas canvas;
        sketch::desktop::CanvasEntity canvas_entity;
        canvas_entity.id = QStringLiteral("unsafe"); canvas_entity.type = QStringLiteral("symbol");
        sketch::desktop::CanvasSvgSymbol artwork;
        artwork.document = nested("marker", 65);
        artwork.view_box = artwork.footprint_view_box = QRectF(0, 0, 10, 10);
        artwork.width_metres = artwork.depth_metres = 1;
        canvas_entity.svg_symbol = artwork;
        canvas.setGridEnabled(false); canvas.setEntities({canvas_entity});
        QPainter output_painter(&image);
        rejects([&] { canvas.renderSceneAt(output_painter, QRectF(image.rect()), 10, {0, 0}, Qt::transparent); });
        output_painter.end();
        optional_guide_regressions();
        const QByteArray profiled = "<svg xmlns='http://www.w3.org/2000/svg' data-vertex-style='white-outline-2'><rect width='10' height='10' fill='#ffffff'/></svg>";
        require(!sketch::desktop::colored_symbol_svg(profiled, {}).isEmpty(), "ordinary palette profile refused");
        auto excessive_palette = nested("g", 65);
        excessive_palette.replace("<svg ", "<svg data-vertex-style='white-outline-2' ");
        rejects([&] { (void)sketch::desktop::colored_symbol_svg(excessive_palette, {}); });
        // Removal of core admission must fail persisted encode/decode checks, not just renderer tests.
        const auto catalog = sketch::default_symbol_catalog();
        const auto found = std::find_if(catalog.begin(), catalog.end(), [](const auto& value) { return value.svg_asset.has_value(); });
        require(found != catalog.end(), "SVG catalog fixture missing");
        sketch::SymbolInstance symbol;
        symbol.id = "admission-symbol"; symbol.symbol_id = found->id;
        symbol.definition = *found; symbol.pinned_svg = ordinary.toStdString();
        sketch::AnnotationState state; state.symbols.push_back(symbol);
        const auto saved = sketch::encode_annotation_state(state, catalog);
        require(sketch::encode_annotation_state(sketch::decode_annotation_state(saved, {}), {}) == saved,
                "historical pinned artwork changed when catalog is unavailable");
        state.symbols.front().pinned_svg = nested("marker", 65).toStdString();
        rejects([&] { (void)sketch::encode_annotation_state(state, catalog); });
        auto hostile = saved; hostile["symbols"][0]["pinned_svg"] = state.symbols.front().pinned_svg;
        rejects([&] { (void)sketch::decode_annotation_state(hostile, catalog); });
        rejects([&] { (void)sketch::desktop::reference_raster_format(QByteArray("<svg/>")); });
        rejects([&] { (void)sketch::desktop::decode_reference_raster(QByteArray("<svg/>")); });
        require(sketch::desktop::reference_raster_format(QByteArray::fromHex("89504e470d0a1a0a")) == "png", "PNG references refused");
        require(sketch::desktop::reference_raster_format(QByteArray::fromHex("ffd8ff")) == "jpeg", "JPEG references refused");
        require(sketch::desktop::reference_raster_format("BM") == "bmp", "BMP references refused");
        require(sketch::desktop::reference_raster_format(QByteArray::fromHex("49492a00")) == "tiff", "little-endian TIFF references refused");
        require(sketch::desktop::reference_raster_format(QByteArray::fromHex("4d4d002a")) == "tiff", "big-endian TIFF references refused");
        if (argc == 2) {
            QDirIterator files(QString::fromLocal8Bit(argv[1]), {"*.svg"}, QDir::Files, QDirIterator::Subdirectories);
            int count = 0;
            while (files.hasNext()) {
                QFile file(files.next()); require(file.open(QIODevice::ReadOnly), "catalog artwork missing");
                QSvgRenderer candidate; require(load_admitted_svg(candidate, file.readAll()), "catalog artwork no longer renderable"); ++count;
            }
            require(count == 345, "complete catalog corpus was not exercised");
        }
        return 0;
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
