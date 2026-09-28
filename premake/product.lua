-- premake/product.lua -- product selection and runtime-context integration.
--
-- This file deliberately does not declare a product project or source list.
-- Product projects can use the selected context once their sources exist,
-- while the existing Amnesia project remains the default target.

local PRODUCT_DEFINITIONS = {
    amnesia = {
        name = "amnesia",
        product = "amnesia",
        runtime = "amnesia",
        project = "Amnesia",
    },
    amfp = {
        name = "amfp",
        product = "amfp",
        runtime = "amfp",
        project = "AmnesiaAMFP",
    },
}

-- Return a stable declaration object rather than making callers duplicate the
-- runtime name. The optional argument is useful for a future project that is
-- declared alongside the selected product.
function product_context(name)
    if type(name) == "table" then name = name.product or name.name end
    name = name or _OPTIONS["product"] or "amnesia"
    local context = PRODUCT_DEFINITIONS[name]
    if not context then
        error("Unknown product context '" .. tostring(name) .. "'")
    end
    return context
end

function product_is_selected(name)
    return product_context().product == product_context(name).product
end

function product_runtime(product_or_context)
    if type(product_or_context) == "table" then
        return product_or_context.product or product_or_context.name or product_or_context.runtime
    end
    return product_context(product_or_context).runtime
end

-- The selected context is metadata only until a product project opts into it.
-- This is what keeps premake/amnesia.lua, tools.lua, and tests.lua on the
-- existing output layout even when a future product is selected.
PRODUCT_CONTEXT = product_context()
PRODUCT_RUNTIME_CONTEXT = PRODUCT_CONTEXT

-- external.lua creates the concrete context (including its per-product paths)
-- after the workspace exists. Product projects can use this helper immediately
-- before configuring their project, or the top-level script can use it to
-- predeclare the selected product's staging wrappers.
function product_external_context(product_or_context)
    local context = product_or_context or PRODUCT_CONTEXT
    local product = product_runtime(context)
    if type(external_runtime_context) ~= "function" then
        error("product_external_context must be called after premake/external.lua")
    end
    return external_runtime_context(product)
end

function product_activate(product_or_context)
    local context = product_or_context or PRODUCT_CONTEXT
    local external_context = product_external_context(context)
    external_use_product(external_context)
    return context
end

function product_deactivate()
    -- Existing projects always use the legacy Amnesia external context. A
    -- future product project should call product_activate() only around its
    -- own declarations, then call this before tools/tests are loaded.
    if type(external_use_product) == "function" then
        external_use_product("amnesia")
    end
end

-- Declare product-specific staging projects without changing the implicit
-- context used by the current Amnesia/tools/tests declarations. The selected
-- context is therefore useful today for generated staging projects and is
-- ready for a future premake/<product>.lua project to consume.
function product_declare_staging_projects()
    if PRODUCT_CONTEXT.product == "amnesia" then return end

    product_activate(PRODUCT_CONTEXT)
    if type(agility_declare_staging_projects) == "function" then
        agility_declare_staging_projects(PRODUCT_CONTEXT.product)
    end
    product_deactivate()
end
