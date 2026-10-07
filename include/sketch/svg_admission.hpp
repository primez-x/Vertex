#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {
class SvgAdmissionError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};
// Preserve the existing pinned-artwork byte limit. Count the SVG root as depth
// one; 64 also matches the existing palette XML limit. Bound admitted nesting
// before native construction/destruction without filtering artwork nodes.
inline constexpr std::size_t svg_document_byte_limit = 262144;
inline constexpr std::size_t svg_element_depth_limit = 64;
// Maximum conservative expanded draw-tree node visits per instance. This
// includes definition containment, inherited resources and marker multiplicity.
inline constexpr std::size_t svg_resource_expansion_limit = 65536;

namespace svg_admission_detail {
inline void require(bool value, const char* message) {
    if (!value) throw SvgAdmissionError(message);
}
inline bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline std::string folded(std::string_view text) {
    std::string result(text);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return result;
}
inline void resource_text(std::string_view text) {
    const auto source = folded(text);
    // Retain the existing annotation resource/active-content policy. Do not
    // impose the palette's narrower artwork vocabulary on historical SVGs.
    for (const auto* token : {"<!doctype", "<!entity", "<script", "foreignobject", "<image", "href",
         "@import", "javascript:", "data:", "file:", "onload", "onerror", "onclick", "onmouse",
         "onfocus", "onbegin", "onend", "onrepeat", "http://www.w3.org/2001/xinclude"})
        require(source.find(token) == std::string::npos, "SVG contains active or external content");
    for (auto pos = source.find("url("); pos != std::string::npos; pos = source.find("url(", pos + 4)) {
        auto content = pos + 4;
        while (content < source.size() && (space(source[content]) || source[content] == '\'' || source[content] == '"')) ++content;
        require(content < source.size() && source[content] == '#', "SVG URL must reference an internal fragment");
    }
}
// Predefined/numeric references do not expand recursively. Decode UTF-8 so
// character references cannot evade policy or internal resource ID lookup.
// XML conformance and namespaces remain the streaming reader's responsibility.
inline void append_utf8(std::string& output,unsigned value) {
    if (value<0x80) output+=static_cast<char>(value);
    else if (value<0x800) {output+=static_cast<char>(0xc0|(value>>6));output+=static_cast<char>(0x80|(value&63));}
    else if (value<0x10000) {output+=static_cast<char>(0xe0|(value>>12));output+=static_cast<char>(0x80|((value>>6)&63));output+=static_cast<char>(0x80|(value&63));}
    else {output+=static_cast<char>(0xf0|(value>>18));output+=static_cast<char>(0x80|((value>>12)&63));output+=static_cast<char>(0x80|((value>>6)&63));output+=static_cast<char>(0x80|(value&63));}
}
inline std::string decoded_references(std::string_view text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t pos = 0; pos < text.size(); ++pos) {
        if (text[pos] != '&') { decoded += text[pos]; continue; }
        const auto end = text.find(';', pos + 1);
        require(end != std::string_view::npos, "Malformed SVG entity reference");
        const auto ref = text.substr(pos + 1, end - pos - 1);
        unsigned value = 0;
        if (ref == "amp") value = '&'; else if (ref == "lt") value = '<';
        else if (ref == "gt") value = '>'; else if (ref == "apos") value = '\'';
        else if (ref == "quot") value = '"';
        else {
            require(ref.starts_with('#'), "SVG contains an undeclared entity reference");
            const bool hex = ref.size() > 1 && ref[1] == 'x';
            const auto digits = ref.substr(hex ? 2 : 1);
            require(!digits.empty(), "Malformed SVG character reference");
            for (const auto c : digits) {
                const auto digit = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                require(digit >= 0 && digit < (hex ? 16 : 10), "Malformed SVG character reference");
                require(value <= (0x10ffffU - static_cast<unsigned>(digit)) / (hex ? 16U : 10U), "Invalid SVG character reference");
                value = value * (hex ? 16U : 10U) + static_cast<unsigned>(digit);
            }
            require(value == 9 || value == 10 || value == 13 || (value >= 32 && value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff) && value != 0xfffe && value != 0xffff), "Invalid SVG character reference");
        }
        append_utf8(decoded,value);
        pos = end;
    }
    return decoded;
}
inline void references(std::string_view text, bool check_resources = true) {
    const auto decoded=decoded_references(text);
    if (check_resources) resource_text(decoded);
}
// A conservative work bound, separate from XML nesting: every containment
// edge and internal resource reference contributes to the estimated draw tree.
// Definitions count too. Saturation and leaf-first evaluation never recurse.
struct ResourceUse { std::string id, property; };
struct ResourceNode {
    std::string element, id, classes, geometry, stylesheet;
    std::size_t parent;
    std::vector<ResourceUse> resources;
};
inline std::string attribute_text(std::string_view source) {
    // Match XML's literal whitespace normalization before decoding references.
    // Numeric character references preserve their own whitespace code points.
    std::string normalized;
    for (std::size_t pos=0;pos<source.size();++pos) {
        const auto c=source[pos];
        if (c=='\r' && pos+1<source.size() && source[pos+1]=='\n') ++pos;
        normalized+=(c=='\r' || c=='\n' || c=='\t') ? ' ' : c;
    }
    return decoded_references(normalized);
}
inline std::string css_text(std::string_view source) {
    std::string result;
    const auto hex = [](char c) { return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1; };
    for (std::size_t pos=0; pos<source.size(); ++pos) {
        if (source.substr(pos).starts_with("/*")) {
            const auto end=source.find("*/",pos+2);
            require(end!=std::string_view::npos,"SVG contains an incomplete CSS comment");
            pos=end+1; continue;
        }
        if (source[pos]!='\\') { result+=source[pos]; continue; }
        require(++pos<source.size(),"SVG contains an incomplete CSS escape");
        if (source[pos]=='\n' || source[pos]=='\r') {
            if (source[pos]=='\r' && pos+1<source.size() && source[pos+1]=='\n') ++pos;
            continue;
        }
        unsigned value=0; std::size_t digits=0;
        while (pos<source.size() && digits<6 && hex(source[pos])>=0) {
            value=value*16+static_cast<unsigned>(hex(source[pos])); ++pos; ++digits;
        }
        if (!digits) { result+=source[pos]; continue; }
        require(value>0 && value<=0x10ffff && !(value>=0xd800 && value<=0xdfff),"SVG contains an invalid CSS escape");
        append_utf8(result,value);
        if (pos<source.size() && space(source[pos])) {
            if (source[pos]=='\r' && pos+1<source.size() && source[pos+1]=='\n') ++pos;
        } else --pos;
    }
    // Qt preprocesses escapes before its CSS comment tokenizer. A delimiter
    // introduced by an escape must not become an uncounted native declaration.
    require(result.find("/*")==std::string::npos,"SVG CSS cannot introduce comments through escapes; use explicit attributes or comment-free styles");
    resource_text(result);
    return result;
}
inline std::string_view trimmed(std::string_view text) {
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return text;
}
inline constexpr std::string_view resource_unicode_spaces[]{
    "\xc2\x85", "\xc2\xa0", "\xe1\x9a\x80", "\xe2\x80\x80",
    "\xe2\x80\x81", "\xe2\x80\x82", "\xe2\x80\x83", "\xe2\x80\x84",
    "\xe2\x80\x85", "\xe2\x80\x86", "\xe2\x80\x87", "\xe2\x80\x88",
    "\xe2\x80\x89", "\xe2\x80\x8a", "\xe2\x80\xa8", "\xe2\x80\xa9",
    "\xe2\x80\xaf", "\xe2\x81\x9f", "\xe3\x80\x80"};
inline std::size_t resource_space_prefix(std::string_view text) {
    if (text.empty()) return 0;
    if (space(text.front()) || text.front()=='\f' || text.front()=='\v') return 1;
    for (const auto candidate:resource_unicode_spaces)
        if (text.starts_with(candidate)) return candidate.size();
    return 0;
}
inline std::string_view trimmed_resource(std::string_view text) {
    // QStringView::trimmed() uses QChar Unicode whitespace, including NBSP.
    // Decode those BMP whitespace sequences without depending on Qt in core.
    while (const auto bytes=resource_space_prefix(text)) text.remove_prefix(bytes);
    for (;;) {
        if (text.empty()) break;
        if (space(text.back()) || text.back()=='\f' || text.back()=='\v') {text.remove_suffix(1);continue;}
        bool removed=false;
        for (const auto candidate:resource_unicode_spaces) {
            if (text.ends_with(candidate)) {text.remove_suffix(candidate.size());removed=true;break;}
        }
        if (!removed) break;
    }
    return text;
}
inline std::vector<ResourceUse> resource_uses(std::string_view text, std::string property) {
    // Presentation attributes are literal XML values. CSS declarations have
    // already been unescaped once by their caller; decoding again would change
    // a literal backslash in the resource ID and hide a native dependency.
    const std::string decoded(text);
    resource_text(decoded);
    const auto lower=folded(decoded);
    std::vector<ResourceUse> result;
    const auto append_fragment=[&](std::string_view body) {
        auto id=trimmed_resource(body);
        if (!id.empty() && (id.front()=='\'' || id.front()=='"')) {
            require(id.size()>=2 && id.back()==id.front(),"SVG contains a malformed resource URL");
            id=trimmed_resource(id.substr(1,id.size()-2));
        }
        require(id.size()>1 && id.front()=='#',"SVG URL must reference a nonempty internal fragment");
        result.push_back({std::string(id.substr(1)),property});
    };
    // Qt's extended attributes accept '(#id)' as well as 'url(#id)'.
    // Count the former explicitly; an absent keyword never means absent work.
    const auto raw=trimmed_resource(decoded);
    if ((property=="mask" || property=="filter" || property.starts_with("marker")) && raw.starts_with('(')) {
        const auto end=raw.find(')',1);
        require(end!=std::string_view::npos,"SVG contains an incomplete resource URL");
        append_fragment(raw.substr(1,end-1));
    }
    for (std::size_t pos=0; (pos=lower.find("url",pos))!=std::string::npos; ++pos) {
        auto begin=pos+3;
        while (const auto bytes=resource_space_prefix(std::string_view(decoded).substr(begin))) begin+=bytes;
        if (begin==lower.size() || lower[begin]!='(') continue;
        const auto end=decoded.find(')',++begin);
        require(end!=std::string::npos,"SVG contains an incomplete resource URL");
        append_fragment(std::string_view(decoded).substr(begin,end-begin));
        pos=end;
    }
    return result;
}
inline std::vector<ResourceUse> declarations(std::string_view source, bool normalized = false) {
    const auto decoded=normalized ? std::string(source) : css_text(source);
    std::vector<ResourceUse> result;
    std::size_t begin=0;
    while (begin<decoded.size()) {
        const auto end=decoded.find(';',begin);
        const auto item=std::string_view(decoded).substr(begin,end==std::string::npos ? decoded.size()-begin : end-begin);
        const auto colon=item.find(':');
        if (colon!=std::string_view::npos) {
            auto uses=resource_uses(item.substr(colon+1),folded(trimmed_resource(item.substr(0,colon))));
            result.insert(result.end(),uses.begin(),uses.end());
        } else require(resource_uses(item,"").empty(),"SVG resource style requires a named property");
        if (end==std::string::npos) break;
        begin=end+1;
    }
    require(result.empty() || (source.find("/*")==std::string_view::npos && decoded.find("/*")==std::string::npos),
        "SVG resource declarations cannot contain CSS comments; use explicit attributes or comment-free styles");
    return result;
}
inline bool paint_node(std::string_view name) {
    return name!="defs" && name!="style" && name!="title" && name!="desc" && name!="metadata" &&
        name!="linearGradient" && name!="radialGradient" && name!="stop";
}
inline bool selector_matches(std::string_view selector,const ResourceNode& node) {
    // Common type/class/ID compounds and combinators are supported. Using only
    // the terminal compound overestimates descendant/sibling matches safely.
    require(std::all_of(selector.begin(),selector.end(),[](char c) {
        return static_cast<unsigned char>(c)<128 && (space(c) || c=='\f' ||
            (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
            std::string_view("_-.#*>+~").find(c)!=std::string_view::npos);
    }),"SVG resource stylesheet selector is unsupported; use ASCII type, class or ID selectors");
    selector=trimmed_resource(selector);
    const auto terminal=selector.find_last_of(" \t\r\n\f>+~");
    if (terminal!=std::string_view::npos) selector=selector.substr(terminal+1);
    require(!selector.empty() && selector.find_first_of(":[]|@\\\"'") == std::string_view::npos,
        "SVG resource stylesheet selector is unsupported; use type, class or ID selectors");
    bool matches=true;
    std::size_t pos=0;
    while (pos<selector.size()) {
        const char kind=selector[pos];
        if (kind=='.' || kind=='#') ++pos;
        const auto end=selector.find_first_of(".#",pos);
        const auto name=selector.substr(pos,end==std::string_view::npos ? selector.size()-pos : end-pos);
        require(!name.empty(),"SVG contains an incomplete resource stylesheet selector");
        if (kind=='#') matches=matches && name==node.id;
        else if (kind=='.') {
            bool present=false;
            std::size_t word=0;
            while (word<node.classes.size()) {
                while (word<node.classes.size()) {
                    const auto bytes=resource_space_prefix(std::string_view(node.classes).substr(word));
                    if (!bytes) break;
                    word+=bytes;
                }
                const auto begin=word;
                while (word<node.classes.size() && !resource_space_prefix(std::string_view(node.classes).substr(word))) ++word;
                present=present || name==std::string_view(node.classes).substr(begin,word-begin);
            }
            matches=matches && present;
        } else matches=matches && (name=="*" || folded(name)==folded(node.element));
        if (end==std::string_view::npos) break;
        pos=end;
    }
    return matches;
}
inline void bounded_resources(std::vector<ResourceNode>& nodes) {
    std::size_t work=0;
    const auto charge=[&] {
        require(++work<=svg_resource_expansion_limit,"SVG resource admission exceeds its work budget; simplify styles or resource references");
    };
    for (std::size_t owner=0; owner<nodes.size(); ++owner) {
        if (nodes[owner].stylesheet.empty()) continue;
        const auto css=css_text(nodes[owner].stylesheet);
        if (resource_uses(css,"").empty()) continue;
        require(nodes[owner].stylesheet.find("/*")==std::string::npos && css.find("/*")==std::string::npos,
            "SVG resource stylesheet cannot contain CSS comments; use explicit attributes or comment-free styles");
        std::size_t pos=0;
        while (pos<css.size()) {
            const auto open=css.find('{',pos),close=css.find('}',open==std::string::npos ? pos : open+1);
            require(open!=std::string::npos && close!=std::string::npos && css.find('{',open+1)>close,
                "SVG resource stylesheet must use flat rules; nested or incomplete rules are unsupported");
            const auto uses=declarations(std::string_view(css).substr(open+1,close-open-1),true);
            if (!uses.empty()) {
                auto selectors=trimmed(std::string_view(css).substr(pos,open-pos));
                std::size_t start=0;
                do {
                    const auto end=selectors.find(',',start);
                    const auto selector=selectors.substr(start,end==std::string_view::npos ? selectors.size()-start : end-start);
                    for (auto& node:nodes) {
                        charge();
                        if (selector_matches(selector,node))
                            for (const auto& use:uses) { charge(); node.resources.push_back(use); }
                    }
                    if (end==std::string_view::npos) break;
                    start=end+1;
                } while (start<selectors.size());
            }
            pos=close+1;
            if (trimmed(std::string_view(css).substr(pos)).empty()) break;
        }
    }
    std::map<std::string,std::size_t,std::less<>> ids;
    for (std::size_t i=0;i<nodes.size();++i)
        if (!nodes[i].id.empty()) require(ids.emplace(nodes[i].id,i).second,"SVG contains duplicate resource IDs");
    struct Edge {std::size_t owner,weight;};
    std::vector<std::vector<Edge>> incoming(nodes.size());
    std::vector<std::size_t> pending(nodes.size()),cost(nodes.size(),1),ready;
    const auto edge=[&](std::size_t owner,std::size_t target,std::size_t weight) {
        charge(); ++pending[owner]; incoming[target].push_back({owner,weight});
    };
    for (std::size_t i=0;i<nodes.size();++i) {
        if (nodes[i].parent<nodes.size()) edge(nodes[i].parent,i,1);
        if (!paint_node(nodes[i].element)) continue;
        // Overcount ancestor resources rather than assuming a CSS inheritance
        // subset. Nonpainting definitions do not acquire inherited paint.
        for (auto ancestor=i; ancestor<nodes.size(); ancestor=nodes[ancestor].parent) {
            for (const auto& use:nodes[ancestor].resources) {
                const auto target=ids.find(use.id);
                // Never interpret an unresolved graph edge as zero work: XML,
                // CSS and native URL parsing can otherwise disagree on its ID.
                require(target!=ids.end(),"SVG references an unresolved internal fragment; repair the resource ID");
                std::size_t weight=1;
                if (use.property.starts_with("marker")) {
                    if (use.property=="marker-end")
                        weight=std::max<std::size_t>(1,static_cast<std::size_t>(std::count(nodes[i].geometry.begin(),nodes[i].geometry.end(),'M')+
                            std::count(nodes[i].geometry.begin(),nodes[i].geometry.end(),'m')));
                    // Qt 6.11 polyline/polygon midpoint handling can use the
                    // start-marker ID, so start and mid both use a conservative
                    // vertex bound rather than assuming one start invocation.
                    else weight=nodes[i].geometry.size()+1;
                }
                edge(i,target->second,weight);
            }
        }
    }
    for (std::size_t i=0;i<nodes.size();++i) if (!pending[i]) ready.push_back(i);
    std::size_t completed=0;
    while (!ready.empty()) {
        const auto node=ready.back();ready.pop_back();++completed;
        for (const auto& dependency:incoming[node]) {
            require(cost[node]<=(svg_resource_expansion_limit-cost[dependency.owner])/dependency.weight,
                "SVG resource expansion exceeds 65536 node visits; simplify markers, masks or patterns");
            cost[dependency.owner]+=cost[node]*dependency.weight;
            if (!--pending[dependency.owner]) ready.push_back(dependency.owner);
        }
    }
    require(completed==nodes.size(),"SVG contains a cyclic internal resource graph; remove recursive resource references");
}

} // namespace svg_admission_detail

// Bounded, nonrecursive lexical structure/resource preflight for headless
// import/save/reopen. This is deliberately not a general XML parser. Every
// renderer additionally runs a conforming streaming XML reader before load.
inline void validate_svg_structure(std::string_view source) {
    using namespace svg_admission_detail;
    require(!source.empty() && source.size() <= svg_document_byte_limit, "SVG is empty or exceeds the per-instance limit");
    require(source.find('\0') == std::string_view::npos, "SVG contains a NUL byte");
    resource_text(source);
    std::vector<std::string_view> stack;
    std::vector<std::size_t> node_stack;
    std::vector<ResourceNode> nodes;
    bool root_seen = false;
    std::size_t pos = source.starts_with("\xef\xbb\xbf") ? 3 : 0;
    const auto skip_space = [&] { while (pos < source.size() && space(source[pos])) ++pos; };
    const auto name = [&]() {
        const auto begin = pos;
        while (pos < source.size() && !space(source[pos]) &&
               source[pos] != '/' && source[pos] != '>' && source[pos] != '=' && source[pos] != '<' && source[pos] != '\'' && source[pos] != '"') ++pos;
        require(pos > begin, "Malformed SVG element or attribute name");
        return source.substr(begin, pos - begin);
    };
    while (pos < source.size()) {
        if (source[pos] != '<') {
            const auto end = source.find('<', pos);
            const auto text = source.substr(pos, end == std::string_view::npos ? source.size() - pos : end - pos);
            if (stack.empty()) require(std::all_of(text.begin(), text.end(), space), "SVG has text outside its root");
            else {
                const auto decoded=decoded_references(text);
                if (nodes[node_stack.back()].element=="style") nodes[node_stack.back()].stylesheet+=decoded;
            }
            pos += text.size(); continue;
        }
        if (source.substr(pos).starts_with("<!--")) {
            const auto end = source.find("-->", pos + 4);
            require(end != std::string_view::npos && source.substr(pos + 4, end - pos - 4).find("--") == std::string_view::npos, "Malformed SVG comment");
            pos = end + 3; continue;
        }
        if (source.substr(pos).starts_with("<![CDATA[")) {
            const auto end = source.find("]]>", pos + 9);
            require(!stack.empty() && end != std::string_view::npos, "Malformed SVG CDATA");
            if (nodes[node_stack.back()].element=="style") nodes[node_stack.back()].stylesheet+=source.substr(pos+9,end-pos-9);
            pos = end + 3; continue;
        }
        if (source.substr(pos).starts_with("<?")) {
            const auto end = source.find("?>", pos + 2);
            require(!root_seen && stack.empty() && source.substr(pos, 5) == "<?xml" && pos + 5 < source.size() && space(source[pos + 5]) && end != std::string_view::npos,
                    "SVG cannot contain processing instructions");
            pos = end + 2; continue;
        }
        require(!source.substr(pos).starts_with("<!"), "SVG cannot contain declarations or external includes");
        ++pos;
        const bool closing = pos < source.size() && source[pos] == '/';
        if (closing) ++pos;
        const auto element = name();
        if (closing) {
            skip_space(); require(pos < source.size() && source[pos++] == '>' && !stack.empty() && stack.back() == element, "Malformed SVG closing element");
            stack.pop_back(); node_stack.pop_back(); continue;
        }
        require(stack.size() < svg_element_depth_limit, "SVG exceeds the XML element depth limit");
        const auto local = element.substr(element.find_last_of(':') == std::string_view::npos ? 0 : element.find_last_of(':') + 1);
        resource_text(std::string("<") + std::string(local) + " ");
        if (stack.empty()) { require(!root_seen && local == "svg", "SVG must have exactly one SVG root"); root_seen = true; }
        require(nodes.size()<svg_resource_expansion_limit,"SVG exceeds the resource node budget");
        const auto node_index=nodes.size();
        nodes.push_back({std::string(local),{},{},{},{},node_stack.empty() ? svg_resource_expansion_limit : node_stack.back(),{}});
        std::string xml_id;
        std::set<std::string_view> attributes;
        bool self_closing = false;
        for (;;) {
            const auto before = pos; skip_space();
            require(pos < source.size(), "Truncated SVG opening element");
            if (source[pos] == '>') { ++pos; break; }
            if (source[pos] == '/') { ++pos; require(pos < source.size() && source[pos++] == '>', "Malformed SVG empty element"); self_closing = true; break; }
            require(pos > before, "SVG attributes must be separated by whitespace");
            const auto key = name(); require(attributes.insert(key).second, "SVG contains duplicate attributes");
            skip_space(); require(pos < source.size() && source[pos++] == '=', "Malformed SVG attribute"); skip_space();
            require(pos < source.size() && (source[pos] == '\'' || source[pos] == '"'), "SVG attribute must be quoted");
            const auto quote = source[pos++]; const auto end = source.find(quote, pos);
            require(end != std::string_view::npos && source.substr(pos, end - pos).find('<') == std::string_view::npos, "Malformed SVG attribute value");
            const auto value=attribute_text(source.substr(pos,end-pos));
            resource_text(value);
            auto& node=nodes[node_index];
            // XML names are case-sensitive even though CSS type selectors are
            // not. Uppercase ID/CLASS attributes cannot override Qt's id/class.
            const std::string attribute(key);
            if (attribute=="id") node.id=value;
            else if (attribute=="xml:id") xml_id=value;
            else if (attribute=="class") node.classes=value;
            else if (attribute=="d" || attribute=="points") node.geometry+=value;
            const auto uses=attribute=="style" ? declarations(value) : resource_uses(value,attribute);
            node.resources.insert(node.resources.end(),uses.begin(),uses.end());
            pos = end + 1;
        }
        // Qt's resource definitions accept xml:id when ordinary id is empty.
        if (nodes[node_index].id.empty()) nodes[node_index].id=std::move(xml_id);
        if (!self_closing) { stack.push_back(element); node_stack.push_back(node_index); }
    }
    require(root_seen && stack.empty(), "Malformed or incomplete SVG structure");
    bounded_resources(nodes);
}
} // namespace sketch
