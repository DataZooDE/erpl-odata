#pragma once
#include "duckdb_compat.hpp"

#include "duckdb.hpp"
#include "tracing.hpp"
#include "tinyxml2.h"
#include "datazoo/oauth2/odata_version.hpp"
#include <string>
#include <vector>
#include <map>
#include <regex>
#include <variant>
#include <iostream>
#include <memory>
#include <chrono>

// Cross-platform string comparison
#ifdef _WIN32
#include <string.h>
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

namespace erpl_web 
{

// OData Version enum ---------------------------------------------------
//
// Defined by datazoo-oauth2's http_client.hpp, not here. The extraction took
// HttpRequest -- which carries SetODataVersion()/AddODataVersionHeaders() --
// into the library, and pulling this 2500-line OData header (plus tinyxml2)
// along with it was rejected as scope creep, so the library declares the enum
// locally instead. Two definitions of erpl_web::ODataVersion would then break
// every translation unit that sees both headers, and most of this repo does.
//
// Defining it in exactly one place and including that place is the whole fix;
// the enumerators are identical (UNKNOWN, V2, V4), so nothing changes. The
// include itself is at file scope near the top, and it is the tiny
// odata_version.hpp rather than http_client.hpp: the latter does
// `using namespace duckdb;` at global scope, and pulling that in EARLIER than
// the old include order did made `EnumType` ambiguous between duckdb::EnumType
// and erpl_web::EnumType in test_odata_edm.cpp.

// PrimitiveType class ---------------------------------------------------
class PrimitiveType 
{

public:
    PrimitiveType(const std::string &class_name) 
        : name(class_name) 
    {}

    static PrimitiveType FromString(const std::string& class_name) {
        if (!IsValidPrimitiveType(class_name)) {
            throw std::invalid_argument("Invalid primitive type: " + class_name);
        }
        return PrimitiveType(class_name);
    }

    static bool IsValidPrimitiveType(const std::string & class_name) {
        static const std::vector<std::string> primitive_types = {
            "Edm.Binary",
            "Edm.Boolean",
            "Edm.Byte",
            "Edm.Date",
            "Edm.DateTime",
            "Edm.DateTimeOffset",
            "Edm.Decimal",
            "Edm.Double",
            "Edm.Duration",
            "Edm.Guid",
            "Edm.Int16",
            "Edm.Int32",
            "Edm.Int64",
            "Edm.SByte",
            "Edm.Single",
            "Edm.Stream",
            "Edm.String",
            "Edm.Time",
            "Edm.TimeOfDay",
            "Edm.Geography",
            "Edm.GeographyPoint",
            "Edm.GeographyLineString",
            "Edm.GeographyPolygon",
            "Edm.GeographyMultiPoint",
            "Edm.GeographyMultiLineString",
            "Edm.GeographyMultiPolygon",
            "Edm.GeographyCollection",
            "Edm.Geometry",
            "Edm.GeometryPoint",
            "Edm.GeometryLineString",
            "Edm.GeometryPolygon",
            "Edm.GeometryMultiPoint",
            "Edm.GeometryMultiLineString",
            "Edm.GeometryMultiPolygon",
            "Edm.GeometryCollection"
        };

        return std::find(primitive_types.begin(), primitive_types.end(), class_name) != primitive_types.end();
    }

    bool operator==(const PrimitiveType& other) const {
        return name == other.name;
    }

    bool operator!=(const PrimitiveType& other) const {
        return name != other.name;
    }

public:
    std::string name;
    std::string ToString() const { return name; }
};

const PrimitiveType Binary("Edm.Binary");
const PrimitiveType Boolean("Edm.Boolean");
const PrimitiveType Byte("Edm.Byte");
const PrimitiveType Date("Edm.Date");
const PrimitiveType DateTime("Edm.DateTime");
const PrimitiveType DateTimeOffset("Edm.DateTimeOffset");
const PrimitiveType Decimal("Edm.Decimal");
const PrimitiveType Double("Edm.Double");
const PrimitiveType Duration("Edm.Duration");
const PrimitiveType Guid("Edm.Guid");
const PrimitiveType Int16("Edm.Int16");
const PrimitiveType Int32("Edm.Int32");
const PrimitiveType Int64("Edm.Int64");
const PrimitiveType SByte("Edm.SByte");
const PrimitiveType Single("Edm.Single");
const PrimitiveType Stream("Edm.Stream");
const PrimitiveType String("Edm.String");
const PrimitiveType Time("Edm.Time");
const PrimitiveType TimeOfDay("Edm.TimeOfDay");
const PrimitiveType Geography("Edm.Geography");
const PrimitiveType GeographyPoint("Edm.GeographyPoint");
const PrimitiveType GeographyLineString("Edm.GeographyLineString");
const PrimitiveType GeographyPolygon("Edm.GeographyPolygon");
const PrimitiveType GeographyMultiPoint("Edm.GeographyMultiPoint");
const PrimitiveType GeographyMultiLineString("Edm.GeographyMultiLineString");
const PrimitiveType GeographyMultiPolygon("Edm.GeographyMultiPolygon");
const PrimitiveType GeographyCollection("Edm.GeographyCollection");
const PrimitiveType Geometry("Edm.Geometry");
const PrimitiveType GeometryPoint("Edm.GeometryPoint");
const PrimitiveType GeometryLineString("Edm.GeometryLineString");
const PrimitiveType GeometryPolygon("Edm.GeometryPolygon");
const PrimitiveType GeometryMultiPoint("Edm.GeometryMultiPoint");
const PrimitiveType GeometryMultiLineString("Edm.GeometryMultiLineString");
const PrimitiveType GeometryMultiPolygon("Edm.GeometryMultiPolygon");
const PrimitiveType GeometryCollection("Edm.GeometryCollection");

// EdmItemBase class -----------------------------------------------------
template<typename TItem>
class EdmItemBase
{
public:
    EdmItemBase* parent = nullptr;
};

// Annotation class -------------------------------------------------------
class Annotation : public EdmItemBase<Annotation>
{
public:
    Annotation() {}
    
    static Annotation FromXml(const tinyxml2::XMLElement& element) {
        Annotation annotation;

        // Parse Term attribute
        const char* term_attr = element.Attribute("Term");
        if (term_attr) {
            annotation.term = term_attr;
        }

        // Parse Qualifier attribute
        const char* qualifier_attr = element.Attribute("Qualifier");
        if (qualifier_attr) {
            annotation.qualifier = qualifier_attr;
        }

        // Parse Path attribute
        const char* path_attr = element.Attribute("Path");
        if (path_attr) {
            annotation.path = path_attr;
        }

        // Parse Annotation elements
        for (const tinyxml2::XMLElement* annotation_el = element.FirstChildElement("Annotation");
            annotation_el != nullptr;
            annotation_el = annotation_el->NextSiblingElement("Annotation")) 
        {
            annotation.annotationType = annotation_el->Name();
        }

        return annotation;
    }

public:
    std::string annotationType = "Unknown";
    std::string term;
    std::string qualifier;
    std::string path;
};

// Annotations class -------------------------------------------------------
class Annotations : public EdmItemBase<Annotations> 
{
public:
    Annotations() {}

    static Annotations FromXml(const tinyxml2::XMLElement& element) {
        Annotations annotations;

        // Parse Target attribute
        const char* target_attr = element.Attribute("Target");
        if (target_attr) {
            annotations.target = target_attr;
        }

        // Parse Qualifier attribute
        const char* qualifier_attr = element.Attribute("Qualifier");
        if (qualifier_attr) {
            annotations.qualifier = qualifier_attr;
        }

        // Parse Annotation elements
        for (const tinyxml2::XMLElement* annotation_el = element.FirstChildElement("Annotation");
            annotation_el != nullptr;
            annotation_el = annotation_el->NextSiblingElement("Annotation")) 
        {
            annotations.annotations.push_back(Annotation::FromXml(*annotation_el));
        }

        return annotations;
    }

public:
    std::string target;
    std::string qualifier;
    std::vector<Annotation> annotations;

};

// Parameter class ---------------------------------------------------------
class FunctionParameter : public EdmItemBase<FunctionParameter>  
{
public:
    FunctionParameter() : nullable(true), SRID(0) {}

    static FunctionParameter FromXml(const tinyxml2::XMLElement& element) {
        try {
            FunctionParameter parameter;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                parameter.name = name_attr;
            }

            // Parse Type attribute
            const char* type_attr = element.Attribute("Type");
            if (type_attr) {
                parameter.type = type_attr;
            }

            // Parse Nullable attribute
            const char* nullable_attr = element.Attribute("Nullable");
            if (nullable_attr) {
                parameter.nullable = std::string(nullable_attr) == "true";
            }

            // Parse DefaultValue attribute
            const char* default_value_attr = element.Attribute("DefaultValue");
            if (default_value_attr) {
                parameter.default_value = default_value_attr;
            }

            // Parse MaxLength attribute
            const char* max_length_attr = element.Attribute("MaxLength");
            if (max_length_attr && strlen(max_length_attr) > 0) {
                try {
                    parameter.max_length = std::stoi(max_length_attr);
                } catch (const std::exception&) {
                    // If conversion fails, set to default value
                    parameter.max_length = -1;
                }
            }

            // Parse Precision attribute
            const char* precision_attr = element.Attribute("Precision");
            if (precision_attr && strlen(precision_attr) > 0) {
                try {
                    parameter.precision = std::stoi(precision_attr);
                } catch (const std::exception&) {
                    // If conversion fails, set to default value
                    parameter.precision = -1;
                }
            }

            // Parse Scale attribute
            const char* scale_attr = element.Attribute("Scale");
            if (scale_attr && strcasecmp(scale_attr, "variable") == 0) {
                parameter.scale = -1;
            } else if (scale_attr && strlen(scale_attr) > 0) {
                try {
                    parameter.scale = std::stoi(scale_attr);
                } catch (const std::exception&) {
                    // If conversion fails, set to default value
                    parameter.scale = -1;
                }
            }

            // Parse SRID attribute
            const char* SRID_attr = element.Attribute("SRID");
            if (SRID_attr && strlen(SRID_attr) > 0) {
                try {
                    parameter.SRID = std::stoi(SRID_attr);
                } catch (const std::exception&) {
                    // If conversion fails, set to default value
                    parameter.SRID = 0;
                }
            }

            // Parse Unicode attribute
            const char* unicode_attr = element.Attribute("Unicode");
            if (unicode_attr) {
                parameter.unicode = std::string(unicode_attr) == "true";
            }

            // Parse Annotation elements
            for (const tinyxml2::XMLElement* annotation_el = element.FirstChildElement("Annotation");
                annotation_el != nullptr;
                annotation_el = annotation_el->NextSiblingElement("Annotation")) 
            {
                auto annotation = Annotation::FromXml(*annotation_el);
                parameter.annotations.push_back(annotation);
            }

            return parameter;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing FunctionParameter: " << e.what() << std::endl;
            std::cerr << "FunctionParameter XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
    std::string type;
    bool nullable = true;
    // -1 means "attribute absent from the CSDL"; see the note on Property. (GitHub #73)
    int max_length = -1;
    int precision = -1;
    int scale = -1;
    int SRID = 0;
    bool unicode = true;
    std::string default_value;
    std::vector<Annotation> annotations;
};

// Invokable class
template<typename TItem>
class Invokable : public EdmItemBase<TItem> 
{
public:
    Invokable() {}
};

// Function class ---------------------------------------------------------
class Function : public Invokable<Function> 
{
public:
    Function() {}

    static Function FromXml(const tinyxml2::XMLElement& element)
    {
        try {
            Function function;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                function.name = name_attr;
            }

            const char *returnType_attr = element.Attribute("ReturnType");
            if (returnType_attr) {
                function.return_type = returnType_attr;
            }

            // Parse Parameter elements
            for (const tinyxml2::XMLElement* parameter_el = element.FirstChildElement("Parameter");
                parameter_el != nullptr;
                parameter_el = parameter_el->NextSiblingElement("Parameter")) 
            { 
                auto parameter = FunctionParameter::FromXml(*parameter_el);
                function.parameters.push_back(parameter);
            }

            // Parse Annotation elements
            for (const tinyxml2::XMLElement* annotation_el = element.FirstChildElement("Annotation");
                annotation_el != nullptr;
                annotation_el = annotation_el->NextSiblingElement("Annotation")) 
            {
                auto annotation = Annotation::FromXml(*annotation_el);
                function.annotations.push_back(annotation);
            }

            return function;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing Function: " << e.what() << std::endl;
            std::cerr << "Function XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
    std::string return_type;
    std::vector<FunctionParameter> parameters;
    std::vector<Annotation> annotations;
};

// EnumMember class -------------------------------------------------------
class EnumMember : public EdmItemBase<EnumMember>
{
public:
    EnumMember() : value(0) {}


    static EnumMember FromXml(const tinyxml2::XMLElement& element)
    {
        try {
            EnumMember member;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                member.name = name_attr;
            }

            // Parse Value attribute
            const char* value_attr = element.Attribute("Value");
            if (value_attr && strlen(value_attr) > 0) {
                try {
                    member.value = std::stoi(value_attr);
                } catch (const std::exception&) {
                    // If conversion fails, set to default value
                    member.value = 0;
                }
            }

            // Parse Annotation elements
            for (const tinyxml2::XMLElement* annotation_el = element.FirstChildElement("Annotation");
                annotation_el != nullptr;
                annotation_el = annotation_el->NextSiblingElement("Annotation")) 
            {
                auto annotation = Annotation::FromXml(*annotation_el);
                member.annotations.push_back(annotation);
            }

            return member;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing EnumMember: " << e.what() << std::endl;
            std::cerr << "EnumMember XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
    int value;
    std::vector<Annotation> annotations;
};

// EnumType class ---------------------------------------------------------
class EnumType : public EdmItemBase<EnumType> 
{
public:
    EnumType() : underlying_type(erpl_web::Int32), is_flags(false) {}

    static EnumType FromXml(const tinyxml2::XMLElement& element) {
        try {
            EnumType enumType;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                enumType.name = name_attr;
            }

            // Parse UnderlyingType attribute
            const char* underlyingType_attr = element.Attribute("UnderlyingType");
            if (underlyingType_attr) {
                auto underlying_type = PrimitiveType(underlyingType_attr);
                enumType.underlying_type = underlying_type;
            }

            // Parse IsFlags attribute
            const char* isFlags_attr = element.Attribute("IsFlags");
            if (isFlags_attr) {
                enumType.is_flags = std::string(isFlags_attr) == "true";
            }

            // Parse Member elements
            for (const tinyxml2::XMLElement* member_el = element.FirstChildElement("Member");
                member_el != nullptr;
                member_el = member_el->NextSiblingElement("Member")) 
            {
                EnumMember member;
                const char* name_attr = member_el->Attribute("Name");
                if (name_attr) {
                    member.name = name_attr;
                }
                const char* value_attr = member_el->Attribute("Value");
                if (value_attr && strlen(value_attr) > 0) {
                    try {
                        member.value = std::stoi(value_attr);
                    } catch (const std::exception&) {
                        // If conversion fails, set to default value
                        member.value = 0;
                    }
                }
                enumType.members.push_back(member);
            }

            // Parse Annotation elements
            for (const tinyxml2::XMLElement* annotation_el = element.FirstChildElement("Annotation");
                annotation_el != nullptr;
                annotation_el = annotation_el->NextSiblingElement("Annotation")) 
            {
                auto annotation = Annotation::FromXml(*annotation_el);
                enumType.annotations.push_back(annotation);
            }

            return enumType;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing EnumType: " << e.what() << std::endl;
            std::cerr << "EnumType XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
    PrimitiveType underlying_type;
    bool is_flags = false;
    std::vector<EnumMember> members;
    std::vector<Annotation> annotations;
};

// ReferentialConstraint class
class ReferentialConstraint : public EdmItemBase<ReferentialConstraint> 
{
public:
    ReferentialConstraint() {}

    static ReferentialConstraint FromXml(const tinyxml2::XMLElement& element) {
        try {
            ReferentialConstraint referential_constraint;

            // Parse Property attribute
            const char* property_attr = element.Attribute("Property");
            if (property_attr) {
                referential_constraint.property = property_attr;
            }

            // Parse ReferencedProperty attribute
            const char* referencedProperty_attr = element.Attribute("ReferencedProperty");
            if (referencedProperty_attr) {
                referential_constraint.referenced_property = referencedProperty_attr;
            }

            return referential_constraint;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing ReferentialConstraint: " << e.what() << std::endl;
            std::cerr << "ReferentialConstraint XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string property;
    std::string referenced_property;
};

// NavigationProperty class -----------------------------------------------
class NavigationProperty : public EdmItemBase<NavigationProperty> 
{

public:
    NavigationProperty() : nullable(true), contains_target(false) {}

    static NavigationProperty FromXml(const tinyxml2::XMLElement& element) {
        try {
        NavigationProperty nav_prop;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                nav_prop.name = name_attr;
            }

            // Parse Type attribute (OData v4)
            const char* type_attr = element.Attribute("Type");
            if (type_attr) {
                nav_prop.type = type_attr;
            }

            // Parse OData v2 specific attributes
            const char* relationship_attr = element.Attribute("Relationship");
            if (relationship_attr) {
                nav_prop.relationship = relationship_attr;
            }

            const char* from_role_attr = element.Attribute("FromRole");
            if (from_role_attr) {
                nav_prop.from_role = from_role_attr;
            }

            const char* to_role_attr = element.Attribute("ToRole");
            if (to_role_attr) {
                nav_prop.to_role = to_role_attr;
            }

            // Parse Nullable attribute
            const char* nullable_attr = element.Attribute("Nullable");
            if (nullable_attr) {
                nav_prop.nullable = std::string(nullable_attr) == "true";
            }

            // Parse Partner attribute
            const char* partner_attr = element.Attribute("Partner");
            if (partner_attr) {
                nav_prop.partner = partner_attr;
            }

            // Parse ContainsTarget attribute
            const char* containsTarget_attr = element.Attribute("ContainsTarget");
            if (containsTarget_attr) {
                nav_prop.contains_target = std::string(containsTarget_attr) == "true";
            }

            // Parse ReferentialConstraint elements
            for (const tinyxml2::XMLElement* referentialConstraint_el = element.FirstChildElement("ReferentialConstraint");
                referentialConstraint_el != nullptr;
                referentialConstraint_el = referentialConstraint_el->NextSiblingElement("ReferentialConstraint")) 
            {
                auto referential_constraint = ReferentialConstraint::FromXml(*referentialConstraint_el);
                nav_prop.referential_constraints.push_back(referential_constraint);
            }

            // Parse Annotation elements
            for (const tinyxml2::XMLElement* annotation_el = element.FirstChildElement("Annotation");
                annotation_el != nullptr;
                annotation_el = annotation_el->NextSiblingElement("Annotation")) 
            {
                auto annotation = Annotation::FromXml(*annotation_el);
                nav_prop.annotations.push_back(annotation);
            }

            return nav_prop;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing NavigationProperty: " << e.what() << std::endl;
            std::cerr << "NavigationProperty XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
    std::string type;
    bool nullable = true;
    std::string partner;
    bool contains_target = false;
    
    // OData v2 specific attributes
    std::string relationship;
    std::string from_role;
    std::string to_role;
    
    std::vector<ReferentialConstraint> referential_constraints;
    std::vector<Annotation> annotations;

};

// AssociationEnd class --------------------------------------------------
class AssociationEnd : public EdmItemBase<AssociationEnd>
{
public:
    std::string type;
    std::string multiplicity;
    std::string role;
};

// AssociationSetEnd class -----------------------------------------------
class AssociationSetEnd : public EdmItemBase<AssociationSetEnd>
{
public:
    std::string entity_set;
    std::string role;
};

// Association class ------------------------------------------------------
class Association : public EdmItemBase<Association> 
{
public:
    Association() {}

    static Association FromXml(const tinyxml2::XMLElement& element) {
        try {
            Association association;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                association.name = name_attr;
            }

            // Parse End elements
            for (const tinyxml2::XMLElement* end_el = element.FirstChildElement("End");
                end_el != nullptr;
                end_el = end_el->NextSiblingElement("End")) 
            {
                AssociationEnd end;
                const char* type_attr = end_el->Attribute("Type");
                if (type_attr) {
                    end.type = type_attr;
                }
                const char* multiplicity_attr = end_el->Attribute("Multiplicity");
                if (multiplicity_attr) {
                    end.multiplicity = multiplicity_attr;
                }
                const char* role_attr = end_el->Attribute("Role");
                if (role_attr) {
                    end.role = role_attr;
                }
                association.ends.push_back(end);
            }

            // Parse ReferentialConstraint elements
            for (const tinyxml2::XMLElement* constraint_el = element.FirstChildElement("ReferentialConstraint");
                constraint_el != nullptr;
                constraint_el = constraint_el->NextSiblingElement("ReferentialConstraint")) 
            {
                association.referential_constraints.push_back(ReferentialConstraint::FromXml(*constraint_el));
            }

            return association;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing Association: " << e.what() << std::endl;
            std::cerr << "Association XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
    std::vector<AssociationEnd> ends;
    std::vector<ReferentialConstraint> referential_constraints;
};

// AssociationSet class --------------------------------------------------
class AssociationSet : public EdmItemBase<AssociationSet> 
{
public:
    AssociationSet() {}

    static AssociationSet FromXml(const tinyxml2::XMLElement& element) {
        try {
            AssociationSet association_set;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                association_set.name = name_attr;
            }

            // Parse Association attribute
            const char* association_attr = element.Attribute("Association");
            if (association_attr) {
                association_set.association = association_attr;
            }

            // Parse End elements (OData v2 uses default namespace)
            for (const tinyxml2::XMLElement* end_el = element.FirstChildElement();
                end_el != nullptr;
                end_el = end_el->NextSiblingElement()) 
            {
                if (std::string(end_el->Name()) == "End") {
                    AssociationSetEnd end;
                    const char* entity_set_attr = end_el->Attribute("EntitySet");
                    if (entity_set_attr) {
                        end.entity_set = entity_set_attr;
                    }
                    const char* role_attr = end_el->Attribute("Role");
                    if (role_attr) {
                        end.role = role_attr;
                    }
                    association_set.ends.push_back(end);
                }
            }

            return association_set;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing AssociationSet: " << e.what() << std::endl;
            std::cerr << "AssociationSet XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
    std::string association;
    std::vector<AssociationSetEnd> ends;
};

// Property class ---------------------------------------------------------
class Property : public EdmItemBase<Property>
{
public:
    Property() : nullable(true), SRID(0) {}

    static Property FromXml(const tinyxml2::XMLElement& element) {
        try {
            Property property;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                property.name = name_attr;
            }

            // Parse Type attribute
            const char* type_attr = element.Attribute("Type");
            if (type_attr) {
                property.type_name = type_attr;
            }

            // Parse Nullable attribute
            const char* nullable_attr = element.Attribute("Nullable");
            if (nullable_attr) {
                property.nullable = std::string(nullable_attr) == "true";
            }

            // Parse DefaultValue attribute
            const char* default_value_attr = element.Attribute("DefaultValue");
            if (default_value_attr) {
                property.default_value = default_value_attr;
            }

            // Parse MaxLength attribute
            const char* max_length_attr = element.Attribute("MaxLength");
            if (max_length_attr && strlen(max_length_attr) > 0) {
                try {
                    property.max_length = std::stoi(max_length_attr);
                } catch (const std::exception&) {
                    // If conversion fails, set to default value
                    property.max_length = -1;
                }
            }

            // Parse FixedLength attribute
            const char* fixed_length_attr = element.Attribute("FixedLength");
            if (fixed_length_attr && strlen(fixed_length_attr) > 0) {
                try {
                    property.fixed_length = std::stoi(fixed_length_attr);
                } catch (const std::exception&) {
                    // If conversion fails, set to default value
                    property.fixed_length = -1;
                }
            }

            // Parse Precision attribute
            const char* precision_attr = element.Attribute("Precision");
            if (precision_attr && strlen(precision_attr) > 0) {
                try {
                    property.precision = std::stoi(precision_attr);
                } catch (const std::exception&) {
                    // If conversion fails, set to default value
                    property.precision = -1;
                }
            }

            // Parse Scale attribute
            const char* scale_attr = element.Attribute("Scale");
            if (scale_attr && strcasecmp(scale_attr, "variable") == 0) {
                property.scale = -1;
            } else if (scale_attr && strlen(scale_attr) > 0) {
                try {
                    property.scale = std::stoi(scale_attr);
                } catch (const std::exception&) {
                    // If conversion fails, set to default value
                    property.scale = -1;
                }
            }

            // Parse SRID attribute
            const char* SRID_attr = element.Attribute("SRID");
            if (SRID_attr && strlen(SRID_attr) > 0) {
                try {
                    property.SRID = std::stoi(SRID_attr);
                } catch (const std::exception&) {
                    // If conversion fails, set to default value
                    property.SRID = 0;
                }
            }

            // Parse Unicode attribute
            const char* unicode_attr = element.Attribute("Unicode");
            if (unicode_attr) {
                property.unicode = std::string(unicode_attr) == "true";
            }

            // Parse Sorting attribute
            const char* sorting_attr = element.Attribute("Sorting");
            if (sorting_attr) {
                property.sorting = sorting_attr;
            }

            // Parse ConcurrencyMode attribute
            const char* concurrencyMode_attr = element.Attribute("ConcurrencyMode");
            if (concurrencyMode_attr) {
                property.concurrencyMode = concurrencyMode_attr;
            }

            // Parse Annotation elements
            for (const tinyxml2::XMLElement* annotation_el = element.FirstChildElement("Annotation");
                annotation_el != nullptr;
                annotation_el = annotation_el->NextSiblingElement("Annotation")) 
            {
                auto annotation = Annotation::FromXml(*annotation_el);
                property.annotations.push_back(annotation);
            }

            return property;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing Property: " << e.what() << std::endl;
            std::cerr << "Property XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
    std::string type_name;
    bool nullable = true;
    std::string default_value;
    // -1 means "attribute absent from the CSDL". Without these initializers every
    // <Property/> that omits MaxLength/FixedLength/Precision/Scale carried indeterminate
    // values into BuildLogicalTypeForProperty(). (GitHub #73)
    int max_length = -1;
    int fixed_length = -1;
    int precision = -1;
    int scale = -1;
    int SRID = 0;
    bool unicode = true;
    std::string sorting;
    std::string concurrencyMode;
    std::vector<Annotation> annotations;
};

// ComplexType class ------------------------------------------------------
class ComplexType : public EdmItemBase<ComplexType> 
{
public:
    ComplexType() : abstract_type(false), open_type(false), has_tream(false) {}

    static ComplexType FromXml(const tinyxml2::XMLElement& element) {
        try {
            ComplexType complex_type;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                complex_type.name = name_attr;
            }

            // Parse BaseType attribute
            const char* baseType_attr = element.Attribute("BaseType");
            if (baseType_attr) {
                complex_type.base_type = baseType_attr;
            }

            // Parse Abstract attribute
            const char* abstract_attr = element.Attribute("Abstract");
            if (abstract_attr) {
                complex_type.abstract_type = std::string(abstract_attr) == "true";
            }

            // Parse OpenType attribute
            const char* openType_attr = element.Attribute("OpenType");
            if (openType_attr) {
                complex_type.open_type = std::string(openType_attr) == "true";
            }

            // Parse HasStream attribute
            const char* hasStream_attr = element.Attribute("HasStream");
            if (hasStream_attr) {
                complex_type.has_tream = std::string(hasStream_attr) == "true";
            }

            // Parse Property elements
            for (const tinyxml2::XMLElement* prop_el = element.FirstChildElement("Property");
                prop_el != nullptr;
                prop_el = prop_el->NextSiblingElement("Property")) 
            {
                auto property = Property::FromXml(*prop_el);
                complex_type.properties.push_back(property);
            }

            // Parse NavigationProperty elements
            for (const tinyxml2::XMLElement* nav_prop_el = element.FirstChildElement("NavigationProperty");
                nav_prop_el != nullptr;
                nav_prop_el = nav_prop_el->NextSiblingElement("NavigationProperty")) 
            {
                auto nav_prop = NavigationProperty::FromXml(*nav_prop_el);
                complex_type.navigation_properties.push_back(nav_prop);
            }

            // Parse Annotation elements
            for (const tinyxml2::XMLElement* annotation_el = element.FirstChildElement("Annotation");
                annotation_el != nullptr;
                annotation_el = annotation_el->NextSiblingElement("Annotation")) 
            {
                auto annotation = Annotation::FromXml(*annotation_el);
                    complex_type.annotations.push_back(annotation);
            }

            return complex_type;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing ComplexType: " << e.what() << std::endl;
            std::cerr << "ComplexType XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
    std::string base_type;
    bool abstract_type = false;
    bool open_type = false;
    bool has_tream = false;
    std::vector<Property> properties;
    std::vector<NavigationProperty> navigation_properties;
    std::vector<Annotation> annotations;
};

// PropertyRef class ------------------------------------------------------
class PropertyRef : public EdmItemBase<PropertyRef>
{

public:
    PropertyRef() {}

    static PropertyRef FromXml(const tinyxml2::XMLElement& element) {
        try {
            PropertyRef property_ref;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                property_ref.name = name_attr;
            }

            return property_ref;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing PropertyRef: " << e.what() << std::endl;
            std::cerr << "PropertyRef XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
};

// Key class --------------------------------------------------------------
class Key : public EdmItemBase<Key> 
{
public:
    Key() {}

    static Key FromXml(const tinyxml2::XMLElement& element) {
        try {
            Key key;

            // Parse PropertyRef elements
            for (const tinyxml2::XMLElement* propertyRef_el = element.FirstChildElement("PropertyRef");
                propertyRef_el != nullptr;
                propertyRef_el = propertyRef_el->NextSiblingElement("PropertyRef")) 
            {
                PropertyRef property_ref;
                const char* name_attr = propertyRef_el->Attribute("Name");
                if (name_attr) {
                    property_ref.name = name_attr;
                }
                key.property_refs.push_back(property_ref);
            }

            return key;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing Key: " << e.what() << std::endl;
            std::cerr << "Key XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::vector<PropertyRef> property_refs;
};

// EntityType class --------------------------------------------------------
class EntityType : public EdmItemBase<EntityType> 
{
public:
    EntityType() : abstract_type(false), open_type(false), hasStream(false) {}

    static EntityType FromXml(const tinyxml2::XMLElement& element) {
        try {
            EntityType entity_type;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                entity_type.name = name_attr;
            }

            // Parse Key element
            const tinyxml2::XMLElement* key_el = element.FirstChildElement("Key");
            if (key_el) {
                entity_type.key = Key::FromXml(*key_el);
            }

            // Parse BaseType attribute
            const char* baseType_attr = element.Attribute("BaseType");
            if (baseType_attr) {
                entity_type.base_type = baseType_attr;
            }

            // Parse Abstract attribute
            const char* abstract_attr = element.Attribute("Abstract");
            if (abstract_attr) {
                entity_type.abstract_type = std::string(abstract_attr) == "true";
            }

            // Parse OpenType attribute
            const char* openType_attr = element.Attribute("OpenType");
            if (openType_attr) {
                entity_type.open_type = std::string(openType_attr) == "true";
            }

            // Parse HasStream attribute
            const char* hasStream_attr = element.Attribute("HasStream");
            if (hasStream_attr) {
                entity_type.hasStream = std::string(hasStream_attr) == "true";
            }

            // Parse Property elements
            for (const tinyxml2::XMLElement* prop_el = element.FirstChildElement("Property");
                prop_el != nullptr;
                prop_el = prop_el->NextSiblingElement("Property")) 
            {
                auto property = Property::FromXml(*prop_el);
                entity_type.properties.push_back(property);
            }

            // Parse NavigationProperty elements
            for (const tinyxml2::XMLElement* nav_prop_el = element.FirstChildElement("NavigationProperty");
                nav_prop_el != nullptr;
                nav_prop_el = nav_prop_el->NextSiblingElement("NavigationProperty")) 
            {
                auto nav_prop = NavigationProperty::FromXml(*nav_prop_el);
                entity_type.navigation_properties.push_back(nav_prop);
            }

            // Parse Annotation elements
            for (const tinyxml2::XMLElement* annotation_el = element.FirstChildElement("Annotation");
                annotation_el != nullptr;
                annotation_el = annotation_el->NextSiblingElement("Annotation")) 
            {
                auto annotation = Annotation::FromXml(*annotation_el);
                entity_type.annotations.push_back(annotation);
            }

            return entity_type;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing EntityType: " << e.what() << std::endl;
            std::cerr << "EntityType XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }


public:
    std::string name;
    Key key;
    std::string base_type;
    bool abstract_type = false;
    bool open_type = false;
    bool hasStream = false;
    std::vector<Property> properties;
    std::vector<NavigationProperty> navigation_properties;
    std::vector<Annotation> annotations;
};

// TypeDefinition class ---------------------------------------------------
class TypeDefinition : public EdmItemBase<TypeDefinition>
{
public:
    TypeDefinition() : underlying_type(erpl_web::Int32), maxLength(0), unicode(false), precision(0), scale(0), SRID(0) {}

    static TypeDefinition FromXml(const tinyxml2::XMLElement& element) {
        try {
            TypeDefinition type_definition;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                type_definition.name = name_attr;
            }

            // Parse UnderlyingType attribute
            const char* underlyingType_attr = element.Attribute("UnderlyingType");
            if (underlyingType_attr) {
                auto underlying_type = PrimitiveType(underlyingType_attr);
                type_definition.underlying_type = underlying_type;
            }

            // Parse MaxLength attribute
            const char* maxLength_attr = element.Attribute("MaxLength");
            if (maxLength_attr && strlen(maxLength_attr) > 0) {
                try {
                    type_definition.maxLength = std::stoi(maxLength_attr);
                } catch (const std::exception&) {
                    // If conversion fails, set to default value
                    type_definition.maxLength = -1;
                }
            }

            // Parse Unicode attribute
            const char* unicode_attr = element.Attribute("Unicode");
            if (unicode_attr) {
                type_definition.unicode = std::string(unicode_attr) == "true";
            }

            // Parse Precision attribute
            const char* precision_attr = element.Attribute("Precision");
            if (precision_attr && strlen(precision_attr) > 0) {
                try {
                    type_definition.precision = std::stoi(precision_attr);
                } catch (const std::exception&) {
                    // If conversion fails, set to default value
                    type_definition.precision = -1;
                }
            }

            // Parse Scale attribute
            const char* scale_attr = element.Attribute("Scale");
            if (scale_attr && strlen(scale_attr) > 0) {
                try {
                    type_definition.scale = std::stoi(scale_attr);
                } catch (const std::exception&) {
                    // If conversion fails, set to default value
                    type_definition.scale = -1;
                }
            }

            // Parse SRID attribute
            const char* SRID_attr = element.Attribute("SRID");
            if (SRID_attr && strlen(SRID_attr) > 0) {
                try {
                    type_definition.SRID = std::stoi(SRID_attr);
                } catch (const std::exception&) {
                    // If conversion fails, set to default value
                    type_definition.SRID = 0;
                }
            }

            // Parse Annotation elements
            for (const tinyxml2::XMLElement* annotation_el = element.FirstChildElement("Annotation");
                annotation_el != nullptr;
                annotation_el = annotation_el->NextSiblingElement("Annotation")) 
            {
                auto annotation = Annotation::FromXml(*annotation_el);
                type_definition.annotations.push_back(annotation);
            }

            return type_definition;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing TypeDefinition: " << e.what() << std::endl;
            std::cerr << "TypeDefinition XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
    PrimitiveType underlying_type;
    int maxLength = 0;
    bool unicode = true;
    int precision = 0;
    int scale = 0;
    int SRID = 0;
    std::vector<Annotation> annotations;
};

// EntitySet class --------------------------------------------------------
class EntitySet : public EdmItemBase<EntitySet> 
{
public:
    EntitySet() {}

    static EntitySet FromXml(const tinyxml2::XMLElement& element) {
        try {
            EntitySet entity_set;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                entity_set.name = name_attr;
            }

            // Parse EntityType attribute
            const char* entityType_attr = element.Attribute("EntityType");
            if (entityType_attr) {
                entity_set.entity_type_name = entityType_attr;
            }

            // Parse Annotation elements
            for (const tinyxml2::XMLElement* annotation_el = element.FirstChildElement("Annotation");
                annotation_el != nullptr;
                annotation_el = annotation_el->NextSiblingElement("Annotation")) 
            {
                auto annotation = Annotation::FromXml(*annotation_el);
                entity_set.annotations.push_back(annotation);
            }

            return entity_set;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing EntitySet: " << e.what() << std::endl;
            std::cerr << "EntitySet XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
    std::string entity_type_name;
    std::vector<Annotation> annotations;
};

// ActionImport class -----------------------------------------------------
class ActionImport : public EdmItemBase<ActionImport> 
{
public:
    ActionImport() {}

    static ActionImport FromXml(const tinyxml2::XMLElement& element) 
    {
        try {
            ActionImport action_import;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                action_import.name = name_attr;
            }

            // Parse Action attribute
            const char* action_attr = element.Attribute("Action");
            if (action_attr) {
                action_import.action = action_attr;
            }

            // Parse Annotation elements
            for (const tinyxml2::XMLElement* annotation_el = element.FirstChildElement("Annotation");
                annotation_el != nullptr;
                annotation_el = annotation_el->NextSiblingElement("Annotation")) 
            {
                auto annotation = Annotation::FromXml(*annotation_el);
                action_import.annotations.push_back(annotation);
            }

            return action_import;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing ActionImport: " << e.what() << std::endl;
            std::cerr << "ActionImport XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
    std::string action;
    std::vector<Annotation> annotations;
};

// FunctionImport class ---------------------------------------------------
class FunctionImport : public EdmItemBase<FunctionImport> 
{
public:
    FunctionImport() : includeInServiceDocument(false) {}

    static FunctionImport FromXml(const tinyxml2::XMLElement& element) 
    {
        try {
            FunctionImport function_import;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                function_import.name = name_attr;
            }

            // Parse Function attribute
            const char* function_attr = element.Attribute("Function");
            if (function_attr) {
                function_import.function = function_attr;
            }

            // Parse IncludeInServiceDocument attribute
            const char* includeInServiceDocument_attr = element.Attribute("IncludeInServiceDocument");
            if (includeInServiceDocument_attr) {
                function_import.includeInServiceDocument = std::string(includeInServiceDocument_attr) == "true";
            }

            // Parse Annotation elements
            for (const tinyxml2::XMLElement* annotation_el = element.FirstChildElement("Annotation");
                annotation_el != nullptr;
                annotation_el = annotation_el->NextSiblingElement("Annotation")) 
            {
                auto annotation = Annotation::FromXml(*annotation_el);
                function_import.annotations.push_back(annotation);
            }

            return function_import;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing FunctionImport: " << e.what() << std::endl;
            std::cerr << "FunctionImport XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
    std::string function;
    bool includeInServiceDocument = false;
    std::vector<Annotation> annotations;

};

// EntityContainer class --------------------------------------------------
class EntityContainer : public EdmItemBase<EntityContainer> 
{
public: 
    EntityContainer() {}

    static EntityContainer FromXml(const tinyxml2::XMLElement& element) 
    {
        try {
            EntityContainer entity_container;

            // Parse Name attribute
            const char* name_attr = element.Attribute("Name");
            if (name_attr) {
                entity_container.name = name_attr;
            }

            // Parse EntitySet elements
            for (const tinyxml2::XMLElement* entitySet_el = element.FirstChildElement("EntitySet");
                entitySet_el != nullptr;
                entitySet_el = entitySet_el->NextSiblingElement("EntitySet")) 
            {
                EntitySet entitySet;
                const char* entitySet_name_attr = entitySet_el->Attribute("Name");
                if (entitySet_name_attr) {
                    entitySet.name = entitySet_name_attr;
                }
                const char* entitySet_entityType_attr = entitySet_el->Attribute("EntityType");
                if (entitySet_entityType_attr) {
                    entitySet.entity_type_name = entitySet_entityType_attr;
                }
                entity_container.entity_sets.push_back(entitySet);
            }

            // Parse AssociationSet elements
            for (const tinyxml2::XMLElement* associationSet_el = element.FirstChildElement("AssociationSet");
                associationSet_el != nullptr;
                associationSet_el = associationSet_el->NextSiblingElement("AssociationSet")) 
            {
                entity_container.association_sets.push_back(AssociationSet::FromXml(*associationSet_el));
            }
            
            // Also try to find AssociationSet elements in the default namespace
            for (const tinyxml2::XMLElement* child_el = element.FirstChildElement();
                child_el != nullptr;
                child_el = child_el->NextSiblingElement()) 
            {
                if (std::string(child_el->Name()) == "AssociationSet") {
                    entity_container.association_sets.push_back(AssociationSet::FromXml(*child_el));
                }
            }

            // Parse ActionImport elements
            for (const tinyxml2::XMLElement* actionImport_el = element.FirstChildElement("ActionImport");
                actionImport_el != nullptr;
                actionImport_el = actionImport_el->NextSiblingElement("ActionImport")) 
            {
                ActionImport actionImport;
                const char* actionImport_name_attr = actionImport_el->Attribute("Name");
                if (actionImport_name_attr) {
                    actionImport.name = actionImport_name_attr;
                }
                const char* actionImport_action_attr = actionImport_el->Attribute("Action");
                if (actionImport_action_attr) {
                    actionImport.action = actionImport_action_attr;
                }
                entity_container.action_imports.push_back(actionImport);
            }

            // Parse FunctionImport elements
            for (const tinyxml2::XMLElement* functionImport_el = element.FirstChildElement("FunctionImport");
                functionImport_el != nullptr;
                functionImport_el = functionImport_el->NextSiblingElement("FunctionImport")) 
            {
                FunctionImport functionImport;
                const char* functionImport_name_attr = functionImport_el->Attribute("Name");
                if (functionImport_name_attr) {
                    functionImport.name = functionImport_name_attr;
                }
                const char* functionImport_function_attr = functionImport_el->Attribute("Function");
                if (functionImport_function_attr) {
                    functionImport.function = functionImport_function_attr;
                }
                entity_container.function_imports.push_back(functionImport);
            }

            return entity_container;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing EntityContainer: " << e.what() << std::endl;
            std::cerr << "EntityContainer XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string name;
    std::vector<EntitySet> entity_sets;
    std::vector<AssociationSet> association_sets;
    std::vector<ActionImport> action_imports;
    std::vector<FunctionImport> function_imports;
};

// Schema class -----------------------------------------------------------

using TypeVariant = std::variant<PrimitiveType, EnumType, TypeDefinition, ComplexType, EntityType>;

class Schema : public EdmItemBase<Schema> 
{
public:
    Schema() {}

    static Schema FromXml(const tinyxml2::XMLElement& element) {
        try {
            Schema schema;

            // Parse Namespace attribute
            try {
                const char* ns_attr = element.Attribute("Namespace");
                if (ns_attr) {
                    schema.ns = ns_attr;
                }
            } catch (const std::runtime_error& e) {
                std::cerr << "Error parsing Namespaces: " << e.what() << std::endl;
                std::cerr << "Schema XML: " << std::endl << element.Value() << std::endl;
                throw e;
            }

            // Parse Alias attribute
            try {
                const char* alias_attr = element.Attribute("Alias");
                if (alias_attr) {
                    schema.alias = alias_attr;
                }
            } catch (const std::runtime_error& e) {
                std::cerr << "Error parsing Alias: " << e.what() << std::endl;
                std::cerr << "Schema XML: " << std::endl << element.Value() << std::endl;
                throw e;
            }

            // Parse EnumType elements
            for (const tinyxml2::XMLElement* enumType_el = element.FirstChildElement("EnumType");
                enumType_el != nullptr;
                enumType_el = enumType_el->NextSiblingElement("EnumType")) 
            {
                schema.enum_types.push_back(EnumType::FromXml(*enumType_el));
            }

            // Parse TypeDefinition elements
            for (const tinyxml2::XMLElement* typeDef_el = element.FirstChildElement("TypeDefinition");
                typeDef_el != nullptr;
                typeDef_el = typeDef_el->NextSiblingElement("TypeDefinition")) 
            {
                schema.type_definitions.push_back(TypeDefinition::FromXml(*typeDef_el));
            }

            // Parse ComplexType elements
            for (const tinyxml2::XMLElement* complexType_el = element.FirstChildElement("ComplexType");
                complexType_el != nullptr;
                complexType_el = complexType_el->NextSiblingElement("ComplexType")) 
            {
                schema.complex_types.push_back(ComplexType::FromXml(*complexType_el));
            }

            // Parse EntityType elements
            for (const tinyxml2::XMLElement* entityType_el = element.FirstChildElement("EntityType");
                entityType_el != nullptr;
                entityType_el = entityType_el->NextSiblingElement("EntityType")) 
            {
                auto entity_type = EntityType::FromXml(*entityType_el);
                schema.entity_types.push_back(entity_type);
            }

            // Parse Association elements (OData v2)
            for (const tinyxml2::XMLElement* association_el = element.FirstChildElement("Association");
                association_el != nullptr;
                association_el = association_el->NextSiblingElement("Association")) 
            {
                schema.associations.push_back(Association::FromXml(*association_el));
            }

            // Parse AssociationSet elements (OData v2)
            for (const tinyxml2::XMLElement* associationSet_el = element.FirstChildElement("AssociationSet");
                associationSet_el != nullptr;
                associationSet_el = associationSet_el->NextSiblingElement("AssociationSet")) 
            {
                schema.association_sets.push_back(AssociationSet::FromXml(*associationSet_el));
            }

            /*
            // Parse Action elements
            for (const tinyxml2::XMLElement* action_el = element.FirstChildElement("Action");
                action_el != nullptr;
                action_el = action_el->NextSiblingElement("Action")) 
            {
                schema.actions.push_back(Action::FromXml(*action_el));
            }
            */

            // Parse Function elements
            for (const tinyxml2::XMLElement* function_el = element.FirstChildElement("Function");
                function_el != nullptr;
                function_el = function_el->NextSiblingElement("Function")) 
            {
                schema.functions.push_back(Function::FromXml(*function_el));
            }

            // Parse EntityContainer elements
            for (const tinyxml2::XMLElement* entityContainer_el = element.FirstChildElement("EntityContainer");
                entityContainer_el != nullptr;
                entityContainer_el = entityContainer_el->NextSiblingElement("EntityContainer")) 
            {
                schema.entity_containers.push_back(EntityContainer::FromXml(*entityContainer_el));
            }

            // Parse Annotations elements
            for (const tinyxml2::XMLElement* annotations_el = element.FirstChildElement("Annotations");
                annotations_el != nullptr;
                annotations_el = annotations_el->NextSiblingElement("Annotations")) 
            {
                schema.annotations.push_back(Annotations::FromXml(*annotations_el));
            }

            // Resolve OData v2 navigation property types from associations
            schema.ResolveV2NavigationPropertyTypes();

            return schema;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing Schema: " << e.what() << std::endl;
            std::cerr << "Schema XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

    TypeVariant FindType(const std::string& type_name) const 
    {
        for (const auto& enum_type : enum_types) {
            if (enum_type.name == type_name) {
                return enum_type;
            }
        }

        for (const auto& type_def : type_definitions) {
            if (type_def.name == type_name) {
                return type_def;
            }
        }

        for (const auto& complex_type : complex_types) {
            if (complex_type.name == type_name) {
                return complex_type;
            }
        }

        for (const auto& entity_type : entity_types) {
            if (entity_type.name == type_name) {
                return entity_type;
            }
        }

        auto primitive_type = PrimitiveType::FromString(type_name);
        return primitive_type;
    }

    // Resolve OData v2 navigation property types from associations
    void ResolveV2NavigationPropertyTypes() {
        for (auto& entity_type : entity_types) {
            for (auto& nav_prop : entity_type.navigation_properties) {
                if (!nav_prop.relationship.empty() && nav_prop.type.empty()) {
                    // Find the association referenced by this navigation property
                    for (const auto& association : associations) {
                        if (association.name == nav_prop.relationship.substr(nav_prop.relationship.find_last_of('.') + 1)) {
                            // Find the end that matches the ToRole
                            for (const auto& end : association.ends) {
                                if (end.role == nav_prop.to_role) {
                                    // Extract the entity type name from the full type path
                                    std::string entity_type_name = end.type.substr(end.type.find_last_of('.') + 1);
                                    
                                    // Set the type based on multiplicity
                                    if (end.multiplicity == "*") {
                                        nav_prop.type = "Collection(" + entity_type_name + ")";
                                    } else {
                                        nav_prop.type = entity_type_name;
                                    }
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

public:
    std::string ns;
    std::string alias;
    std::vector<EnumType> enum_types;
    std::vector<TypeDefinition> type_definitions;
    std::vector<ComplexType> complex_types;
    std::vector<EntityType> entity_types;
    std::vector<Association> associations;
    std::vector<AssociationSet> association_sets;
    std::vector<Function> functions;
    //std::vector<Action> actions;
    std::vector<EntityContainer> entity_containers;
    std::vector<Annotations> annotations;

    
};

// DataServices class -------------------------------------------------------
class DataServices : public EdmItemBase<DataServices> 
{
public:
    DataServices() {}

    static DataServices FromXml(const tinyxml2::XMLElement& element) {
        try {
            DataServices data_svc;

            // Parse Schema elements
            for (const tinyxml2::XMLElement* schema_el = element.FirstChildElement("Schema");
                schema_el != nullptr;
                schema_el = schema_el->NextSiblingElement("Schema")) 
            {
                data_svc.schemas.push_back(Schema::FromXml(*schema_el));
            }

            return data_svc;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing Schema: " << e.what() << std::endl;
            std::cerr << "Schema XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::vector<Schema> schemas;
};

// ReferenceInclude class --------------------------------------------------
class ReferenceInclude : public EdmItemBase<ReferenceInclude> 
{
public:
    ReferenceInclude() {}

    static ReferenceInclude FromXml(const tinyxml2::XMLElement& element) {
        try {
            ReferenceInclude include;

            const char* ns_attr = element.Attribute("Namespace");
            if (ns_attr) {
                include.namespace_ = ns_attr;
            }

            const char* ailas_attr = element.Attribute("Alias");
            if (ailas_attr) {
                include.alias = ailas_attr;
            }

            return include;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing ReferenceInclude: " << e.what() << std::endl;
            std::cerr << "ReferenceInclude XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }

public:
    std::string namespace_;
    std::string alias;
};

// Reference class ----------------------------------------------------------
class Reference : public EdmItemBase<Reference>
{
public:
    Reference() {}

    static Reference FromXml(const tinyxml2::XMLElement& element) {
        try {
            Reference reference;

            // Parse Uri attribute
            const char* uri_attr = element.Attribute("Uri");
            if (uri_attr) {
                reference.uri = uri_attr;
            }

            // Parse Include elements
            for (const tinyxml2::XMLElement* include_el = element.FirstChildElement("Include");
                include_el != nullptr;
                include_el = include_el->NextSiblingElement("Include")) 
            {
                ReferenceInclude include;
                const char* namespaceAttr = include_el->Attribute("Namespace");
                if (namespaceAttr) {
                    include.namespace_ = namespaceAttr;
                }
                reference.includes.push_back(include);
            }

            return reference;
        } catch (const std::runtime_error& e) {
            std::cerr << "Error parsing Reference: " << e.what() << std::endl;
            std::cerr << "Reference XML: " << std::endl << element.Value() << std::endl;
            throw e;
        }
    }
public:
    std::string uri;
    std::vector<ReferenceInclude> includes;
};

// Edmx class --------------------------------------------------------------
class Edmx : public EdmItemBase<Edmx> 
{
public: 
    Edmx() {}

    static Edmx FromXml(const std::string& xml) {
        tinyxml2::XMLDocument doc;
        tinyxml2::XMLError result = doc.Parse(xml.c_str());
        if (result != tinyxml2::XML_SUCCESS) {
            std::stringstream ss;
            ss << "Failed to parse XML [" << tinyxml2::XMLDocument::ErrorIDToName(result) << "]" << std::endl;   
            ss << "Description: " << doc.ErrorStr() << std::endl;
            ss << "Content: " << std::endl << xml << std::endl;
            throw std::runtime_error(ss.str());
        }

        return FromXml(doc);
    }

    static Edmx FromXml(const tinyxml2::XMLDocument& doc) {
        const tinyxml2::XMLElement* edmx_el = doc.RootElement();
        if (edmx_el == nullptr) {
            throw std::runtime_error("Missing Edmx root element");
        }

        // Auto-detect OData version
        const char* version_attr = edmx_el->Attribute("Version");
        if (version_attr) {
            std::string version_str(version_attr);
            if (version_str == "1.0" || version_str == "2.0") {
                return FromXmlV2(doc);
            } else if (version_str == "4.0") {
                return FromXmlV4(doc);
            }
        }

        // Fallback: detect from namespace
        const char* xmlns_attr = edmx_el->Attribute("xmlns");
        if (xmlns_attr) {
            std::string xmlns(xmlns_attr);
            if (xmlns.find("schemas.microsoft.com/ado") != std::string::npos) {
                return FromXmlV2(doc);
            } else if (xmlns.find("docs.oasis-open.org/odata") != std::string::npos) {
                return FromXmlV4(doc);
            }
        }

        // Default to v4 for backward compatibility
        return FromXmlV4(doc);
    }
    
    // Version-specific parsing methods
    static Edmx FromXmlV2(const std::string& xml);
    static Edmx FromXmlV4(const std::string& xml);
    static Edmx FromXmlV2(const tinyxml2::XMLDocument& doc);
    static Edmx FromXmlV4(const tinyxml2::XMLDocument& doc);

    bool IsFullUrl(const std::string& type_name_or_url) const 
    {
        return type_name_or_url.find("http://") != std::string::npos || type_name_or_url.find("https://") != std::string::npos;
    }

    bool IsRelativeMetadataUrl(const std::string& type_name_or_url) const 
    {
        return type_name_or_url.rfind("$metadata", 0) == 0;
    }

    std::string StripUrlIfNecessary(const std::string& type_name_or_url) const 
    {
        if (! IsFullUrl(type_name_or_url) && ! IsRelativeMetadataUrl(type_name_or_url)) 
        {
            return type_name_or_url;
        }

        auto type_name_pos = type_name_or_url.find('#');
        if (type_name_pos != std::string::npos) 
        {
            auto type_name = type_name_or_url.substr(type_name_pos + 1);
            auto type_arg_pos = type_name.find('(');
            if (type_arg_pos != std::string::npos) {
                return type_name.substr(0, type_arg_pos);
            }
            return type_name;
        }

        throw std::runtime_error("Malformed type name or URL: " + type_name_or_url);
    }

    TypeVariant FindType(const std::string& type_name_or_url) const 
    {
        auto type_name = StripUrlIfNecessary(type_name_or_url);

        auto [ns, local_type_name] = SplitNamespace(type_name);
        if (! ns.empty()) {
            for (const auto& schema : data_services.schemas) {
                // Match either full namespace or alias
                if (schema.ns == ns || (!schema.alias.empty() && schema.alias == ns)) {
                    return schema.FindType(local_type_name);
                }
            }
        }

        // No namespace provided or not matched: try resolving by local name across all schemas (V2 often omits namespaces)
        for (const auto& schema : data_services.schemas) {
            // Attempt entity types
            for (const auto &entity_type : schema.entity_types) {
                if (entity_type.name == local_type_name) {
                    return entity_type;
                }
            }
            // Attempt complex types
            for (const auto &complex_type : schema.complex_types) {
                if (complex_type.name == local_type_name) {
                    return complex_type;
                }
            }
            // Attempt enum types
            for (const auto &enum_type : schema.enum_types) {
                if (enum_type.name == local_type_name) {
                    return enum_type;
                }
            }
            // Attempt type definitions
            for (const auto &type_def : schema.type_definitions) {
                if (type_def.name == local_type_name) {
                    return type_def;
                }
            }
        }

        if (PrimitiveType::IsValidPrimitiveType(local_type_name)) {
            return PrimitiveType::FromString(local_type_name);
        }
        
        throw std::runtime_error("Unable to resolve type: " + type_name);
    }

    EntitySet FindEntitySet(const std::string& entity_set_name_or_url) const
    {
        auto entity_set_name = StripUrlIfNecessary(entity_set_name_or_url);

        for (const auto& schema : data_services.schemas) {
            for (const auto& container : schema.entity_containers) {
                for (const auto& entity_set : container.entity_sets) {
                    if (entity_set.name == entity_set_name) {
                        return entity_set;
                    }
                }
            }
        }

        throw std::runtime_error("Unable to resolve entity set: " + entity_set_name);
    }

    std::vector<EntitySet> FindEntitySets() const
    {
        std::vector<EntitySet> entity_sets;
        for (const auto& schema : data_services.schemas) {
            for (const auto& container : schema.entity_containers) {
                entity_sets.insert(entity_sets.end(), container.entity_sets.begin(), container.entity_sets.end());
            }
        }
        return entity_sets;
    }

private:
    static std::tuple<std::string, std::string> SplitNamespace(const std::string& type_name) {
        size_t pos = type_name.rfind('.');
        if (pos == std::string::npos) {
            return std::make_tuple("", type_name);
        }

        auto ret = std::make_tuple(type_name.substr(0, pos), type_name.substr(pos + 1));
        if (std::get<0>(ret) == "Edm") {
            return std::make_tuple("", type_name);
        }

        return ret;
    }

public:
    std::string version = "4.0";
    DataServices data_services;
    std::vector<Reference> references;

    // OData version support
    ODataVersion GetVersion() const { return version_enum; }
    void SetVersion(ODataVersion version) { version_enum = version; }

private:
    ODataVersion version_enum = ODataVersion::V4;
};

// TypeVariant Handling ----------------------------------------------------

class DuckTypeConverter 
{
    public:
        explicit DuckTypeConverter(const Edmx &edmx) : edmx(edmx) {}

        // The single EDM primitive -> DuckDB type mapping. Everything that needs to know
        // what "Edm.Int32" means goes through here (or through the string-valued sibling
        // below, which reads the same table); there used to be three hand-written copies,
        // which is how Edm.Byte stayed wrong in all of them at once (GitHub #68).
        static duckdb::LogicalType ConvertEdmPrimitiveStringToLogicalType(const std::string &type_name);

        // The widest DECIMAL DuckDB has. CSDL leaves an absent Precision unbounded, so this
        // is the closest representable reading of "unbounded".
        static constexpr int32_t MAX_DECIMAL_WIDTH = 38;

        // Build the DuckDB type for an Edm.Decimal property from its CSDL facets.
        //
        // CSDL says an absent Scale defaults to 0, so DECIMAL(p,0) is technically conformant --
        // but Dataverse and SAP CDS routinely publish money columns as Precision-only, and
        // DECIMAL(p,0) silently rounds 19.99 to 20. Losing the cents is a worse failure than
        // losing exactness, so an absent (or "variable") Scale maps to DOUBLE instead; an
        // explicitly declared Scale -- including Scale="0" -- still yields DECIMAL(p,s).
        // Property::scale defaults to -1 ("absent"), which is what makes the two
        // distinguishable at all. (GitHub #80, relies on GitHub #73)
        //
        // The Precision side is the mirror image, and used to be wrong in the same way.
        // An absent Precision is UNBOUNDED in CSDL, not 18: defaulting it to 18 against a
        // declared Scale="18" -- how TOPdesk's reporting API publishes every money column --
        // produced DECIMAL(18,18), eighteen fractional digits with no room for a single
        // integral one, so every value >= 1 failed to cast and was returned as NULL. Both an
        // absent Precision and a declared one that leaves no integral digits therefore widen
        // to the maximum, keeping the scale the service declared about its own data; only a
        // scale too wide for any DuckDB DECIMAL falls back to DOUBLE. (GitHub #254)
        static duckdb::LogicalType BuildDecimalLogicalType(const Property &property) {
            if (property.scale < 0) {
                return duckdb::LogicalTypeId::DOUBLE;
            }
            if (property.scale >= MAX_DECIMAL_WIDTH) {
                return duckdb::LogicalTypeId::DOUBLE;
            }

            const int32_t scale = property.scale;
            int32_t precision = property.precision > 0 ? property.precision : MAX_DECIMAL_WIDTH;
            if (precision > MAX_DECIMAL_WIDTH || precision <= scale) {
                precision = MAX_DECIMAL_WIDTH;
            }
            return duckdb::LogicalType::DECIMAL(static_cast<uint8_t>(precision), static_cast<uint8_t>(scale));
        }

        // Central property-aware mapping (handles Decimal p/s and Collection(...))
        static duckdb::LogicalType BuildLogicalTypeForProperty(const Property &property, const Edmx &edmx) {
            // Detect Collection(T)
            std::regex collection_regex("Collection\\(([^\\)]+)\\)");
            std::smatch match;
            bool is_collection = false;
            std::string type_name = property.type_name;
            if (std::regex_search(type_name, match, collection_regex)) {
                is_collection = true;
                type_name = match[1];
            }

            duckdb::LogicalType duck_type;
            if (type_name == "Edm.Decimal") {
                duck_type = BuildDecimalLogicalType(property);
            } else if (type_name.rfind("Edm.", 0) == 0) {
                // Primitive
                duck_type = ConvertEdmPrimitiveStringToLogicalType(type_name);
            } else {
                // Complex/Entity: visit via converter
                DuckTypeConverter converter(edmx);
                auto field_type = edmx.FindType(type_name);
                duck_type = std::visit(converter, field_type);
            }

            if (is_collection) {
                duck_type = duckdb::LogicalType::LIST(duck_type);
            }
            return duck_type;
        }

        // The same mapping expressed as a DuckDB type name, for the catalog functions that
        // report column types as strings.
        static std::string ConvertEdmTypeStringToDuckDbTypeString(const std::string& edm_type);

        duckdb::LogicalType operator()(PrimitiveType &type) const;

        duckdb::LogicalType operator()(EnumType &type) const 
        {
            auto type_enum = duckdb::Vector(duckdb::LogicalType::VARCHAR, type.members.size());
            for (size_t i = 0; i < type.members.size(); i++) {
                type_enum.SetValue(i, duckdb::Value(type.members[i].name));
            }
            
            auto duck_typ = duckdb::LogicalType::ENUM(type.name, type_enum, type.members.size());
            return duck_typ;
        }

        duckdb::LogicalType operator()(const TypeDefinition &type) const 
        {
            // Fallback for TypeDefinition - treat as VARCHAR for now
            return duckdb::LogicalTypeId::VARCHAR;
        }

        duckdb::LogicalType operator()(const ComplexType &type) const 
        {
            duckdb::child_list_t<duckdb::LogicalType> fields;

            if (! type.base_type.empty()) {
                auto base_type = std::get<ComplexType>(edmx.FindType(type.base_type));
                AddPropertiesFromBaseType(fields, base_type);  
            }
            AddPropertiesAsFields(fields, type.properties);
            
            // TODO: Add navigation properties as fields once we resolve the circular reference issue
            // For now, navigation properties are excluded to prevent crashes
            // AddNavigationPropertiesAsFields(fields, type.navigation_properties);

            return duckdb::LogicalType::STRUCT(fields);
        }

        duckdb::LogicalType operator()(const EntityType &type) const 
        {
            duckdb::child_list_t<duckdb::LogicalType> fields;

            if (! type.base_type.empty()) {
                auto base_type = std::get<EntityType>(edmx.FindType(type.base_type));
                AddPropertiesFromBaseType(fields, base_type);  
            }
            AddPropertiesAsFields(fields, type.properties);
            
            // IMPORTANT: Do NOT add navigation properties into entity struct fields used for column types
            // Navigation properties are surfaced as separate expanded columns via $expand.
            // Including them here creates nested structs/lists for nav props and causes parsing/type issues.
            // If needed for other scenarios, add behind a controlled flag.

            return duckdb::LogicalType::STRUCT(fields);
        }

        std::tuple<bool, std::string> ExtractCollectionType(const std::string &type_name) const
        {
            std::regex collection_regex("Collection\\(([^\\)]+)\\)");
            std::smatch match;

            if (std::regex_search(type_name, match, collection_regex)) {
                return std::make_tuple(true, match[1]);
            } else {
                return std::make_tuple(false, type_name);
            }
        }

    private:
        void AddPropertiesAsFields(duckdb::child_list_t<duckdb::LogicalType> &fields, const std::vector<Property> &properties) const
        {
            for (const auto &property : properties) 
            {
                auto [is_collection, type_name] = ExtractCollectionType(property.type_name);
                duckdb::LogicalType duck_type;

                // Special-case Edm.Decimal to honor precision/scale metadata
                if (type_name == "Edm.Decimal") {
                    duck_type = BuildDecimalLogicalType(property);
                } else {
                    auto field_type = edmx.FindType(type_name);
                    duck_type = std::visit(*this, field_type);
                }

                if (is_collection) {
                    duck_type = duckdb::LogicalType::LIST(duck_type);
                }

                fields.emplace_back(duckdb_compat::Ident(property.name), duck_type);
            }
        }

        void AddPropertiesFromBaseType(duckdb::child_list_t<duckdb::LogicalType> &fields, const ComplexType &base_type) const
        {
            auto duck_type = operator()(base_type);
            AddFieldsFromStruct(fields, duck_type);
        }

        void AddPropertiesFromBaseType(duckdb::child_list_t<duckdb::LogicalType> &fields, const EntityType &base_type) const
        {
            auto duck_type = operator()(base_type);
            AddFieldsFromStruct(fields, duck_type);
        }

        void AddFieldsFromStruct(duckdb::child_list_t<duckdb::LogicalType> &fields, const duckdb::LogicalType &duck_type) const
        {
            if (duck_type.id() != duckdb::LogicalTypeId::STRUCT) {
                throw std::runtime_error("Expected STRUCT type");
            }

            auto base_child_list = duckdb::StructType::GetChildTypes(duck_type);
            for (size_t i = 0; i < base_child_list.size(); i++) {
                fields.push_back(base_child_list[i]);
            }
        }

        void AddNavigationPropertiesAsFields(duckdb::child_list_t<duckdb::LogicalType> &fields, const std::vector<NavigationProperty> &navigation_properties) const
        {
            for (const auto &nav_prop : navigation_properties) 
            {
                try {
                    auto [is_collection, type_name] = ExtractCollectionType(nav_prop.type);
                    
                    // Try to find the target type in EDM metadata
                    duckdb::LogicalType field_type;
                    
                    if (type_name.find("Edm.") == 0) {
                        // Primitive type - convert directly
                        field_type = ConvertPrimitiveTypeString(type_name);
                    } else {
                        // Entity or complex type - avoid infinite recursion by using a simple approach
                        // For navigation properties, we'll use a basic STRUCT type to avoid circular references
                        try {
                            // Check if it's a collection type
                            if (is_collection) {
                                // For collections, use LIST of STRUCT with basic fields
                                field_type = duckdb::LogicalType::LIST(duckdb::LogicalTypeId::VARCHAR);
                            } else {
                                // For single entities, use STRUCT with basic fields
                                field_type = duckdb::LogicalTypeId::VARCHAR;
                            }
                        } catch (const std::exception& e) {
                            // If anything goes wrong, fall back to VARCHAR
                            ERPL_TRACE_DEBUG("EDM_TYPE_CONVERSION", "Error processing navigation property type '" + type_name + "', using VARCHAR fallback: " + e.what());
                            field_type = duckdb::LogicalTypeId::VARCHAR;
                        }
                    }

                    if (is_collection) {
                        field_type = duckdb::LogicalType::LIST(field_type);
                    }

                    fields.emplace_back(duckdb_compat::Ident(nav_prop.name), field_type);
                } catch (const std::exception& e) {
                    // If anything goes wrong, fall back to VARCHAR
                    ERPL_TRACE_DEBUG("EDM_TYPE_CONVERSION", "Error processing navigation property '" + nav_prop.name + "', using VARCHAR fallback: " + e.what());
                    fields.emplace_back(duckdb_compat::Ident(nav_prop.name), duckdb::LogicalTypeId::VARCHAR);
                }
            }
        }

        // Thin alias for the single public mapping above; this used to be a verbatim copy
        // of it, which is how Edm.Byte stayed wrong in three places at once. (GitHub #68)
        duckdb::LogicalType ConvertPrimitiveTypeString(const std::string& type_name) const
        {
            return ConvertEdmPrimitiveStringToLogicalType(type_name);
        }

    
    public:
        // Held by const reference: the EDM is shared, potentially across connections
        // via EdmCache, and this converter only reads it.
        const Edmx &edmx;
};

// Centralized OData EDM-based type builder utilities (for expand schema)
class ODataEdmTypeBuilder {
public:
    explicit ODataEdmTypeBuilder(const Edmx &edmx) : edmx(edmx), converter(edmx) {}

    // Resolve (is_collection, target_type_name) for a navigation property on an entity type
    std::pair<bool, std::string> ResolveNavTargetOnEntity(const std::string &entity_type_name, const std::string &nav_prop) const;

    // Build STRUCT type for an entity type (properties only; nav props excluded)
    duckdb::LogicalType BuildEntityStruct(const std::string &entity_type_name) const;

    // Build expanded column type for top navigation with nested children
    // Example: topNavProp = "DefaultSystem", nestedChildren = {"Services"}
    // Returns STRUCT(SystemAlias VARCHAR, Description VARCHAR, Services LIST(STRUCT(...)))
    duckdb::LogicalType BuildExpandedColumnType(const std::string &root_entity_type_name,
                                                const std::string &top_nav_prop,
                                                const std::vector<std::string> &nested_children) const;

private:
    const Edmx &edmx;
    DuckTypeConverter converter;
};

// Process-global cache of parsed $metadata documents, keyed by metadata URL.
//
// Ownership: entries are handed out as shared_ptr<const Edmx> snapshots rather than as
// pointers into the map. The previous optional_ptr<Edmx> was borrowed from a map entry
// after the lock had already been released, so a concurrent Set() on the same URL --
// two connections attaching or reading the same service is the normal case -- destroyed
// the Edmx the first caller was still reading. A shared_ptr keeps the snapshot alive for
// exactly as long as somebody holds it, and Set() replaces the map slot rather than the
// object, so readers are never disturbed. The payload is const because it is shared.
class EdmCache
{
public:
    // A cached document is considered stale after this long. $metadata is not immutable:
    // a service redeployment changes it, and without an expiry the entry survives for the
    // lifetime of the process. See GitHub #106.
    static constexpr int64_t DEFAULT_ENTRY_LIFETIME_SECONDS = 900;

    static EdmCache& GetInstance();

    EdmCache(const EdmCache&) = delete;
    EdmCache& operator=(const EdmCache&) = delete;

    // Returns a snapshot, or nullptr when the URL is unknown or its entry has expired.
    std::shared_ptr<const Edmx> Get(const std::string& metadata_url);

    // Stores a snapshot and returns it, so a caller that has just parsed a document can
    // hold the cached instance instead of keeping a private copy of it.
    std::shared_ptr<const Edmx> Set(const std::string& metadata_url, Edmx edmx);

    // Drops one entry (e.g. after a metadata request failed against a redeployed service).
    void Invalidate(const std::string& metadata_url);

    // Drops every entry. Mainly for tests and for a future `erpl_odata_clear_cache` pragma.
    void Clear();

    // Number of entries currently held, expired ones included.
    size_t Size() const;

    // Entry lifetime. Zero expires entries immediately; a negative value disables expiry.
    void SetEntryLifetime(std::chrono::seconds lifetime);
    std::chrono::seconds GetEntryLifetime() const;

private:
    EdmCache() = default;

    struct Entry {
        std::shared_ptr<const Edmx> edmx;
        std::chrono::steady_clock::time_point stored_at;
    };

    // Caller must hold cache_lock.
    bool IsExpired(const Entry& entry, std::chrono::steady_clock::time_point now) const;
    void EvictExpired(std::chrono::steady_clock::time_point now);

    mutable std::mutex cache_lock;
    std::unordered_map<std::string, Entry> cache;
    std::chrono::seconds entry_lifetime{DEFAULT_ENTRY_LIFETIME_SECONDS};

    std::string UrlWithoutFragment(const std::string& url) const;
};


} // namespace erpl_web


