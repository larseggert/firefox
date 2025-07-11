/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

<%!
    from data import Keyword, to_rust_ident, to_phys, to_camel_case, SYSTEM_FONT_LONGHANDS
    from data import (LOGICAL_CORNERS, PHYSICAL_CORNERS, LOGICAL_SIDES,
                      PHYSICAL_SIDES, LOGICAL_SIZES, LOGICAL_AXES)
%>

<%def name="predefined_type(name, type, initial_value, parse_method='parse',
            vector=False, none_value=None, initial_specified_value=None,
            allow_quirks='No', **kwargs)">
    <%def name="predefined_type_inner(name, type, initial_value, parse_method)">
        #[allow(unused_imports)]
        use app_units::Au;
        #[allow(unused_imports)]
        use crate::values::specified::AllowQuirks;
        #[allow(unused_imports)]
        use crate::Zero;
        #[allow(unused_imports)]
        use smallvec::SmallVec;
        pub use crate::values::specified::${type} as SpecifiedValue;
        pub mod computed_value {
            pub use crate::values::computed::${type} as T;
        }
        % if initial_value:
        #[inline] pub fn get_initial_value() -> computed_value::T { ${initial_value} }
        % endif
        % if initial_specified_value:
        #[inline] pub fn get_initial_specified_value() -> SpecifiedValue { ${initial_specified_value} }
        % endif
        #[allow(unused_variables)]
        #[inline]
        pub fn parse<'i, 't>(
            context: &ParserContext,
            input: &mut Parser<'i, 't>,
        ) -> Result<SpecifiedValue, ParseError<'i>> {
            % if allow_quirks != "No":
            specified::${type}::${parse_method}_quirky(context, input, AllowQuirks::${allow_quirks})
            % elif parse_method != "parse":
            specified::${type}::${parse_method}(context, input)
            % else:
            <specified::${type} as crate::parser::Parse>::parse(context, input)
            % endif
        }
    </%def>
    % if vector:
        <%call
            expr="vector_longhand(name, predefined_type=type, allow_empty=not initial_value, none_value=none_value, **kwargs)"
        >
            ${predefined_type_inner(name, type, initial_value, parse_method)}
            % if caller:
            ${caller.body()}
            % endif
        </%call>
    % else:
        <%call expr="longhand(name, predefined_type=type, **kwargs)">
            ${predefined_type_inner(name, type, initial_value, parse_method)}
            % if caller:
            ${caller.body()}
            % endif
        </%call>
    % endif
</%def>

// The setup here is roughly:
//
//  * UnderlyingList is the list that is stored in the computed value. This may
//    be a shared ArcSlice if the property is inherited.
//  * UnderlyingOwnedList is the list that is used for animation.
//  * Specified values always use OwnedSlice, since it's more compact.
//  * computed_value::List is just a convenient alias that you can use for the
//    computed value list, since this is in the computed_value module.
//
// If simple_vector_bindings is true, then we don't use the complex iterator
// machinery and set_foo_from, and just compute the value like any other
// longhand.
<%def name="vector_longhand(name, vector_animation_type=None, allow_empty=False,
                            none_value=None, simple_vector_bindings=False, separator='Comma',
                            **kwargs)">
    <%call expr="longhand(name, vector=True,
                          simple_vector_bindings=simple_vector_bindings, **kwargs)">
        #[allow(unused_imports)]
        use smallvec::SmallVec;

        pub mod single_value {
            #[allow(unused_imports)]
            use cssparser::{Parser, BasicParseError};
            #[allow(unused_imports)]
            use crate::parser::{Parse, ParserContext};
            #[allow(unused_imports)]
            use crate::properties::ShorthandId;
            #[allow(unused_imports)]
            use selectors::parser::SelectorParseErrorKind;
            #[allow(unused_imports)]
            use style_traits::{ParseError, StyleParseErrorKind};
            #[allow(unused_imports)]
            use crate::values::computed::{Context, ToComputedValue};
            #[allow(unused_imports)]
            use crate::values::{computed, specified};
            ${caller.body()}
        }

        /// The definition of the computed value for ${name}.
        pub mod computed_value {
            #[allow(unused_imports)]
            use crate::values::animated::ToAnimatedValue;
            #[allow(unused_imports)]
            use crate::values::resolved::ToResolvedValue;
            pub use super::single_value::computed_value as single_value;
            pub use self::single_value::T as SingleComputedValue;
            % if not allow_empty:
            use smallvec::SmallVec;
            % endif
            use crate::values::computed::ComputedVecIter;

            <%
                is_shared_list = allow_empty and \
                    data.longhands_by_name[name].style_struct.inherited
            %>

            // FIXME(emilio): Add an OwnedNonEmptySlice type, and figure out
            // something for transition-name, which is the only remaining user
            // of NotInitial.
            pub type UnderlyingList<T> =
                % if allow_empty:
                % if data.longhands_by_name[name].style_struct.inherited:
                    crate::ArcSlice<T>;
                % else:
                    crate::OwnedSlice<T>;
                % endif
                % else:
                    SmallVec<[T; 1]>;
                % endif

            pub type UnderlyingOwnedList<T> =
                % if allow_empty:
                    crate::OwnedSlice<T>;
                % else:
                    SmallVec<[T; 1]>;
                % endif


            /// The generic type defining the animated and resolved values for
            /// this property.
            ///
            /// Making this type generic allows the compiler to figure out the
            /// animated value for us, instead of having to implement it
            /// manually for every type we care about.
            #[derive(Clone, Debug, MallocSizeOf, PartialEq, ToAnimatedValue, ToResolvedValue, ToCss)]
            % if separator == "Comma":
            #[css(comma)]
            % endif
            pub struct OwnedList<T>(
                % if not allow_empty:
                #[css(iterable)]
                % else:
                #[css(if_empty = "none", iterable)]
                % endif
                pub UnderlyingOwnedList<T>,
            );

            /// The computed value for this property.
            % if not is_shared_list:
            pub type ComputedList = OwnedList<single_value::T>;
            pub use self::OwnedList as List;
            % else:
            pub use self::ComputedList as List;

            #[derive(Clone, Debug, MallocSizeOf, PartialEq, ToCss)]
            % if separator == "Comma":
            #[css(comma)]
            % endif
            pub struct ComputedList(
                % if not allow_empty:
                #[css(iterable)]
                % else:
                #[css(if_empty = "none", iterable)]
                % endif
                % if is_shared_list:
                #[ignore_malloc_size_of = "Arc"]
                % endif
                pub UnderlyingList<single_value::T>,
            );

            type ResolvedList = <OwnedList<single_value::T> as ToResolvedValue>::ResolvedValue;
            impl ToResolvedValue for ComputedList {
                type ResolvedValue = ResolvedList;

                fn to_resolved_value(self, context: &crate::values::resolved::Context) -> Self::ResolvedValue {
                    OwnedList(
                        self.0
                            .iter()
                            .cloned()
                            .map(|v| v.to_resolved_value(context))
                            .collect()
                    )
                }

                fn from_resolved_value(resolved: Self::ResolvedValue) -> Self {
                    % if not is_shared_list:
                    use std::iter::FromIterator;
                    % endif
                    let iter =
                        resolved.0.into_iter().map(ToResolvedValue::from_resolved_value);
                    ComputedList(UnderlyingList::from_iter(iter))
                }
            }
            % endif

            % if simple_vector_bindings:
            impl From<ComputedList> for UnderlyingList<single_value::T> {
                #[inline]
                fn from(l: ComputedList) -> Self {
                    l.0
                }
            }
            impl From<UnderlyingList<single_value::T>> for ComputedList {
                #[inline]
                fn from(l: UnderlyingList<single_value::T>) -> Self {
                    List(l)
                }
            }
            % endif

            % if vector_animation_type:
            use crate::values::animated::{Animate, ToAnimatedZero, Procedure, lists};
            use crate::values::distance::{SquaredDistance, ComputeSquaredDistance};

            // FIXME(emilio): For some reason rust thinks that this alias is
            // unused, even though it's clearly used below?
            #[allow(unused)]
            type AnimatedList = <OwnedList<single_value::T> as ToAnimatedValue>::AnimatedValue;
            % if is_shared_list:
            impl ToAnimatedValue for ComputedList {
                type AnimatedValue = AnimatedList;

                fn to_animated_value(self, context: &crate::values::animated::Context) -> Self::AnimatedValue {
                    OwnedList(
                        self.0.iter().map(|v| v.clone().to_animated_value(context)).collect()
                    )
                }

                fn from_animated_value(animated: Self::AnimatedValue) -> Self {
                    let iter =
                        animated.0.into_iter().map(ToAnimatedValue::from_animated_value);
                    ComputedList(UnderlyingList::from_iter(iter))
                }
            }
            % endif

            impl ToAnimatedZero for AnimatedList {
                fn to_animated_zero(&self) -> Result<Self, ()> { Err(()) }
            }

            impl Animate for AnimatedList {
                fn animate(
                    &self,
                    other: &Self,
                    procedure: Procedure,
                ) -> Result<Self, ()> {
                    Ok(OwnedList(
                        lists::${vector_animation_type}::animate(&self.0, &other.0, procedure)?
                    ))
                }
            }
            impl ComputeSquaredDistance for AnimatedList {
                fn compute_squared_distance(
                    &self,
                    other: &Self,
                ) -> Result<SquaredDistance, ()> {
                    lists::${vector_animation_type}::squared_distance(&self.0, &other.0)
                }
            }
            % endif

            /// The computed value, effectively a list of single values.
            pub use self::ComputedList as T;

            pub type Iter<'a, 'cx, 'cx_a> = ComputedVecIter<'a, 'cx, 'cx_a, super::single_value::SpecifiedValue>;
        }

        /// The specified value of ${name}.
        #[derive(Clone, Debug, MallocSizeOf, PartialEq, SpecifiedValueInfo, ToCss, ToShmem)]
        % if none_value:
        #[value_info(other_values = "none")]
        % endif
        % if separator == "Comma":
        #[css(comma)]
        % endif
        pub struct SpecifiedValue(
            % if not allow_empty:
            #[css(iterable)]
            % else:
            #[css(if_empty = "none", iterable)]
            % endif
            pub crate::OwnedSlice<single_value::SpecifiedValue>,
        );

        pub fn get_initial_value() -> computed_value::T {
            % if allow_empty:
                computed_value::List(Default::default())
            % else:
                let mut v = SmallVec::new();
                v.push(single_value::get_initial_value());
                computed_value::List(v)
            % endif
        }

        pub fn parse<'i, 't>(
            context: &ParserContext,
            input: &mut Parser<'i, 't>,
        ) -> Result<SpecifiedValue, ParseError<'i>> {
            use style_traits::Separator;

            % if allow_empty or none_value:
            if input.try_parse(|input| input.expect_ident_matching("none")).is_ok() {
                % if allow_empty:
                return Ok(SpecifiedValue(Default::default()))
                % else:
                return Ok(SpecifiedValue(crate::OwnedSlice::from(vec![${none_value}])))
                % endif
            }
            % endif

            let v = style_traits::${separator}::parse(input, |parser| {
                single_value::parse(context, parser)
            })?;
            Ok(SpecifiedValue(v.into()))
        }

        pub use self::single_value::SpecifiedValue as SingleSpecifiedValue;

        % if not simple_vector_bindings and engine == "gecko":
        impl SpecifiedValue {
            fn compute_iter<'a, 'cx, 'cx_a>(
                &'a self,
                context: &'cx Context<'cx_a>,
            ) -> computed_value::Iter<'a, 'cx, 'cx_a> {
                computed_value::Iter::new(context, &self.0)
            }
        }
        % endif

        impl ToComputedValue for SpecifiedValue {
            type ComputedValue = computed_value::T;

            #[inline]
            fn to_computed_value(&self, context: &Context) -> computed_value::T {
                % if not is_shared_list:
                use std::iter::FromIterator;
                % endif
                computed_value::List(computed_value::UnderlyingList::from_iter(
                    self.0.iter().map(|i| i.to_computed_value(context))
                ))
            }

            #[inline]
            fn from_computed_value(computed: &computed_value::T) -> Self {
                let iter = computed.0.iter().map(ToComputedValue::from_computed_value);
                SpecifiedValue(iter.collect())
            }
        }
    </%call>
</%def>
<%def name="longhand(*args, **kwargs)">
    <%
        property = data.declare_longhand(*args, **kwargs)
        if property is None:
            return ""
    %>
    /// ${property.spec}
    pub mod ${property.ident} {
        #[allow(unused_imports)]
        use cssparser::{Parser, BasicParseError, Token};
        #[allow(unused_imports)]
        use crate::parser::{Parse, ParserContext};
        #[allow(unused_imports)]
        use crate::properties::{UnparsedValue, ShorthandId};
        #[allow(unused_imports)]
        use crate::error_reporting::ParseErrorReporter;
        #[allow(unused_imports)]
        use crate::properties::longhands;
        #[allow(unused_imports)]
        use crate::properties::{LonghandId, LonghandIdSet};
        #[allow(unused_imports)]
        use crate::properties::{CSSWideKeyword, ComputedValues, PropertyDeclaration};
        #[allow(unused_imports)]
        use crate::properties::style_structs;
        #[allow(unused_imports)]
        use selectors::parser::SelectorParseErrorKind;
        #[allow(unused_imports)]
        use servo_arc::Arc;
        #[allow(unused_imports)]
        use style_traits::{ParseError, StyleParseErrorKind};
        #[allow(unused_imports)]
        use crate::values::computed::{Context, ToComputedValue};
        #[allow(unused_imports)]
        use crate::values::{computed, generics, specified};
        #[allow(unused_imports)]
        use crate::Atom;
        ${caller.body()}
        #[allow(unused_variables)]
        pub unsafe fn cascade_property(
            declaration: &PropertyDeclaration,
            context: &mut computed::Context,
        ) {
            % if property.logical:
            declaration.debug_crash("Should physicalize before entering here");
            % else:
            context.for_non_inherited_property = ${"false" if property.style_struct.inherited else "true"};
            % if property.logical_group:
            debug_assert_eq!(
                declaration.id().as_longhand().unwrap().logical_group(),
                LonghandId::${property.camel_case}.logical_group(),
            );
            % else:
            debug_assert_eq!(
                declaration.id().as_longhand().unwrap(),
                LonghandId::${property.camel_case},
            );
            % endif
            let specified_value = match *declaration {
                PropertyDeclaration::CSSWideKeyword(ref wk) => {
                    match wk.keyword {
                        % if not property.style_struct.inherited:
                        CSSWideKeyword::Unset |
                        % endif
                        CSSWideKeyword::Initial => {
                            % if not property.style_struct.inherited:
                                declaration.debug_crash("Unexpected initial or unset for non-inherited property");
                            % else:
                                context.builder.reset_${property.ident}();
                            % endif
                        },
                        % if property.style_struct.inherited:
                        CSSWideKeyword::Unset |
                        % endif
                        CSSWideKeyword::Inherit => {
                            % if not property.style_struct.inherited:
                                context.rule_cache_conditions.borrow_mut().set_uncacheable();
                            % endif
                            % if property.is_zoom_dependent():
                                if !context.builder.effective_zoom_for_inheritance.is_one() {
                                    let old_zoom = context.builder.effective_zoom;
                                    context.builder.effective_zoom = context.builder.effective_zoom_for_inheritance;
                                    let computed = context.builder.inherited_style.clone_${property.ident}();
                                    let specified = ToComputedValue::from_computed_value(&computed);
                                    % if property.boxed:
                                    let specified = Box::new(specified);
                                    % endif
                                    let decl = PropertyDeclaration::${property.camel_case}(specified);
                                    cascade_property(&decl, context);
                                    context.builder.effective_zoom = old_zoom;
                                    return;
                                }
                            % endif
                            % if property.style_struct.inherited:
                                declaration.debug_crash("Unexpected inherit or unset for non-zoom-dependent inherited property");
                            % else:
                                context.builder.inherit_${property.ident}();
                            % endif
                        }
                        CSSWideKeyword::RevertLayer |
                        CSSWideKeyword::Revert => {
                            declaration.debug_crash("Found revert/revert-layer not dealt with");
                        },
                    }
                    return;
                },
                #[cfg(debug_assertions)]
                PropertyDeclaration::WithVariables(..) => {
                    declaration.debug_crash("Found variables not substituted");
                    return;
                },
                _ => unsafe {
                    declaration.unchecked_value_as::<${property.specified_type()}>()
                },
            };

            % if property.ident in SYSTEM_FONT_LONGHANDS and engine == "gecko":
            if let Some(sf) = specified_value.get_system() {
                longhands::system_font::resolve_system_font(sf, context);
            }
            % endif

            % if property.is_vector and not property.simple_vector_bindings and engine == "gecko":
                // In the case of a vector property we want to pass down an
                // iterator so that this can be computed without allocation.
                //
                // However, computing requires a context, but the style struct
                // being mutated is on the context. We temporarily remove it,
                // mutate it, and then put it back. Vector longhands cannot
                // touch their own style struct whilst computing, else this will
                // panic.
                let mut s =
                    context.builder.take_${data.current_style_struct.name_lower}();
                {
                    let iter = specified_value.compute_iter(context);
                    s.set_${property.ident}(iter);
                }
                context.builder.put_${data.current_style_struct.name_lower}(s);
            % else:
                % if property.boxed:
                let computed = (**specified_value).to_computed_value(context);
                % else:
                let computed = specified_value.to_computed_value(context);
                % endif
                context.builder.set_${property.ident}(computed)
            % endif
            % endif
        }

        pub fn parse_declared<'i, 't>(
            context: &ParserContext,
            input: &mut Parser<'i, 't>,
        ) -> Result<PropertyDeclaration, ParseError<'i>> {
            % if property.allow_quirks != "No":
                parse_quirky(context, input, specified::AllowQuirks::${property.allow_quirks})
            % else:
                parse(context, input)
            % endif
            % if property.boxed:
                .map(Box::new)
            % endif
                .map(PropertyDeclaration::${property.camel_case})
        }
    }
</%def>

<%def name="gecko_keyword_conversion(keyword, values=None, type='SpecifiedValue', cast_to=None)">
    <%
        if not values:
            values = keyword.values_for(engine)
        maybe_cast = "as %s" % cast_to if cast_to else ""
        const_type = cast_to if cast_to else "u32"
    %>
    #[cfg(feature = "gecko")]
    impl ${type} {
        /// Obtain a specified value from a Gecko keyword value
        ///
        /// Intended for use with presentation attributes, not style structs
        pub fn from_gecko_keyword(kw: u32) -> Self {
            use crate::gecko_bindings::structs;
            % for value in values:
                // We can't match on enum values if we're matching on a u32
                const ${to_rust_ident(value).upper()}: ${const_type}
                    = structs::${keyword.gecko_constant(value)} as ${const_type};
            % endfor
            match kw ${maybe_cast} {
                % for value in values:
                    ${to_rust_ident(value).upper()} => ${type}::${to_camel_case(value)},
                % endfor
                _ => panic!("Found unexpected value in style struct for ${keyword.name} property"),
            }
        }
    }
</%def>

<%def name="gecko_bitflags_conversion(bit_map, gecko_bit_prefix, type, kw_type='u8')">
    #[cfg(feature = "gecko")]
    impl ${type} {
        /// Obtain a specified value from a Gecko keyword value
        ///
        /// Intended for use with presentation attributes, not style structs
        pub fn from_gecko_keyword(kw: ${kw_type}) -> Self {
            % for gecko_bit in bit_map.values():
            use crate::gecko_bindings::structs::${gecko_bit_prefix}${gecko_bit};
            % endfor

            let mut bits = ${type}::empty();
            % for servo_bit, gecko_bit in bit_map.items():
                if kw & (${gecko_bit_prefix}${gecko_bit} as ${kw_type}) != 0 {
                    bits |= ${servo_bit};
                }
            % endfor
            bits
        }

        pub fn to_gecko_keyword(self) -> ${kw_type} {
            % for gecko_bit in bit_map.values():
            use crate::gecko_bindings::structs::${gecko_bit_prefix}${gecko_bit};
            % endfor

            let mut bits: ${kw_type} = 0;
            // FIXME: if we ensure that the Servo bitflags storage is the same
            // as Gecko's one, we can just copy it.
            % for servo_bit, gecko_bit in bit_map.items():
                if self.contains(${servo_bit}) {
                    bits |= ${gecko_bit_prefix}${gecko_bit} as ${kw_type};
                }
            % endfor
            bits
        }
    }
</%def>

<%def name="single_keyword(name, values, vector=False,
            needs_conversion=False, **kwargs)">
    <%
        keyword_kwargs = {a: kwargs.pop(a, None) for a in [
            'gecko_constant_prefix',
            'gecko_enum_prefix',
            'extra_gecko_values',
            'extra_servo_values',
            'gecko_aliases',
            'servo_aliases',
            'custom_consts',
            'gecko_inexhaustive',
            'gecko_strip_moz_prefix',
        ]}
    %>

    <%def name="inner_body(keyword, needs_conversion=False)">
        pub use self::computed_value::T as SpecifiedValue;
        pub mod computed_value {
            #[cfg_attr(feature = "servo", derive(Deserialize, Hash, Serialize))]
            #[derive(Clone, Copy, Debug, Eq, FromPrimitive, MallocSizeOf, Parse, PartialEq, SpecifiedValueInfo, ToAnimatedValue, ToComputedValue, ToCss, ToResolvedValue, ToShmem)]
            pub enum T {
            % for variant in keyword.values_for(engine):
            <%
                aliases = []
                for alias, v in keyword.aliases_for(engine).items():
                    if variant == v:
                        aliases.append(alias)
            %>
            % if aliases:
            #[parse(aliases = "${','.join(sorted(aliases))}")]
            % endif
            ${to_camel_case(variant)},
            % endfor
            }
        }
        #[inline]
        pub fn get_initial_value() -> computed_value::T {
            computed_value::T::${to_camel_case(values.split()[0])}
        }
        #[inline]
        pub fn get_initial_specified_value() -> SpecifiedValue {
            SpecifiedValue::${to_camel_case(values.split()[0])}
        }
        #[inline]
        pub fn parse<'i, 't>(_context: &ParserContext, input: &mut Parser<'i, 't>)
                             -> Result<SpecifiedValue, ParseError<'i>> {
            SpecifiedValue::parse(input)
        }

        % if needs_conversion:
            <%
                conversion_values = keyword.values_for(engine) + list(keyword.aliases_for(engine).keys())
            %>
            ${gecko_keyword_conversion(keyword, values=conversion_values)}
        % endif
    </%def>
    % if vector:
        <%call expr="vector_longhand(name, keyword=Keyword(name, values, **keyword_kwargs), **kwargs)">
            ${inner_body(Keyword(name, values, **keyword_kwargs))}
            % if caller:
            ${caller.body()}
            % endif
        </%call>
    % else:
        <%call expr="longhand(name, keyword=Keyword(name, values, **keyword_kwargs), **kwargs)">
            ${inner_body(Keyword(name, values, **keyword_kwargs),
                         needs_conversion=needs_conversion)}
            % if caller:
            ${caller.body()}
            % endif
        </%call>
    % endif
</%def>

<%def name="shorthand(name, sub_properties, derive_serialize=False,
                      derive_value_info=True, **kwargs)">
<%
    shorthand = data.declare_shorthand(name, sub_properties.split(), **kwargs)
    # mako doesn't accept non-string value in parameters with <% %> form, so
    # we have to workaround it this way.
    if not isinstance(derive_value_info, bool):
        derive_value_info = eval(derive_value_info)
%>
    % if shorthand:
    /// ${shorthand.spec}
    pub mod ${shorthand.ident} {
        use cssparser::Parser;
        use crate::parser::ParserContext;
        use crate::properties::{PropertyDeclaration, SourcePropertyDeclaration, MaybeBoxed, longhands};
        #[allow(unused_imports)]
        use selectors::parser::SelectorParseErrorKind;
        #[allow(unused_imports)]
        use std::fmt::{self, Write};
        #[allow(unused_imports)]
        use style_traits::{ParseError, StyleParseErrorKind};
        #[allow(unused_imports)]
        use style_traits::{CssWriter, KeywordsCollectFn, SpecifiedValueInfo, ToCss};

        % if derive_value_info:
        #[derive(SpecifiedValueInfo)]
        % endif
        pub struct Longhands {
            % for sub_property in shorthand.sub_properties:
                pub ${sub_property.ident}:
                    % if sub_property.boxed:
                        Box<
                    % endif
                    longhands::${sub_property.ident}::SpecifiedValue
                    % if sub_property.boxed:
                        >
                    % endif
                    ,
            % endfor
        }

        /// Represents a serializable set of all of the longhand properties that
        /// correspond to a shorthand.
        % if derive_serialize:
        #[derive(ToCss)]
        % endif
        pub struct LonghandsToSerialize<'a> {
            % for sub_property in shorthand.sub_properties:
                pub ${sub_property.ident}:
                % if sub_property.may_be_disabled_in(shorthand, engine):
                    Option<
                % endif
                    &'a longhands::${sub_property.ident}::SpecifiedValue,
                % if sub_property.may_be_disabled_in(shorthand, engine):
                    >,
                % endif
            % endfor
        }

        impl<'a> LonghandsToSerialize<'a> {
            /// Tries to get a serializable set of longhands given a set of
            /// property declarations.
            pub fn from_iter(iter: impl Iterator<Item = &'a PropertyDeclaration>) -> Result<Self, ()> {
                // Define all of the expected variables that correspond to the shorthand
                % for sub_property in shorthand.sub_properties:
                    let mut ${sub_property.ident} =
                        None::<&'a longhands::${sub_property.ident}::SpecifiedValue>;
                % endfor

                // Attempt to assign the incoming declarations to the expected variables
                for declaration in iter {
                    match *declaration {
                        % for sub_property in shorthand.sub_properties:
                            PropertyDeclaration::${sub_property.camel_case}(ref value) => {
                                ${sub_property.ident} = Some(value)
                            },
                        % endfor
                        _ => {}
                    };
                }

                // If any of the expected variables are missing, return an error
                match (
                    % for sub_property in shorthand.sub_properties:
                        ${sub_property.ident},
                    % endfor
                ) {

                    (
                    % for sub_property in shorthand.sub_properties:
                        % if sub_property.may_be_disabled_in(shorthand, engine):
                        ${sub_property.ident},
                        % else:
                        Some(${sub_property.ident}),
                        % endif
                    % endfor
                    ) =>
                    Ok(LonghandsToSerialize {
                        % for sub_property in shorthand.sub_properties:
                            ${sub_property.ident},
                        % endfor
                    }),
                    _ => Err(())
                }
            }
        }

        /// Parse the given shorthand and fill the result into the
        /// `declarations` vector.
        pub fn parse_into<'i, 't>(
            declarations: &mut SourcePropertyDeclaration,
            context: &ParserContext,
            input: &mut Parser<'i, 't>,
        ) -> Result<(), ParseError<'i>> {
            #[allow(unused_imports)]
            use crate::properties::{NonCustomPropertyId, LonghandId};
            input.parse_entirely(|input| parse_value(context, input)).map(|longhands| {
                % for sub_property in shorthand.sub_properties:
                % if sub_property.may_be_disabled_in(shorthand, engine):
                if NonCustomPropertyId::from(LonghandId::${sub_property.camel_case})
                    .allowed_in_ignoring_rule_type(context) {
                % endif
                    declarations.push(PropertyDeclaration::${sub_property.camel_case}(
                        longhands.${sub_property.ident}
                    ));
                % if sub_property.may_be_disabled_in(shorthand, engine):
                }
                % endif
                % endfor
            })
        }

        /// Try to serialize a given shorthand to a string.
        pub fn to_css(declarations: &[&PropertyDeclaration], dest: &mut crate::str::CssStringWriter) -> fmt::Result {
            match LonghandsToSerialize::from_iter(declarations.iter().cloned()) {
                Ok(longhands) => longhands.to_css(&mut CssWriter::new(dest)),
                Err(_) => Ok(())
            }
        }

        ${caller.body()}
    }
    % endif
</%def>

// A shorthand of kind `<property-1> <property-2>?` where both properties have
// the same type.
<%def name="two_properties_shorthand(
    name,
    first_property,
    second_property,
    parser_function='crate::parser::Parse::parse',
    **kwargs
)">
<%call expr="self.shorthand(name, sub_properties=' '.join([first_property, second_property]), **kwargs)">
    #[allow(unused_imports)]
    use crate::parser::Parse;
    #[allow(unused_imports)]
    use crate::values::specified;

    fn parse_value<'i, 't>(
        context: &ParserContext,
        input: &mut Parser<'i, 't>,
    ) -> Result<Longhands, ParseError<'i>> {
        let parse_one = |c: &ParserContext, input: &mut Parser<'i, 't>| -> Result<
            crate::properties::longhands::${to_rust_ident(first_property)}::SpecifiedValue,
            ParseError<'i>
        > {
            ${parser_function}(c, input)
        };

        let first = parse_one(context, input)?;
        let second =
            input.try_parse(|input| parse_one(context, input)).unwrap_or_else(|_| first.clone());
        Ok(expanded! {
            ${to_rust_ident(first_property)}: first,
            ${to_rust_ident(second_property)}: second,
        })
    }

    impl<'a> ToCss for LonghandsToSerialize<'a>  {
        fn to_css<W>(&self, dest: &mut CssWriter<W>) -> fmt::Result where W: fmt::Write {
            let first = &self.${to_rust_ident(first_property)};
            let second = &self.${to_rust_ident(second_property)};

            first.to_css(dest)?;
            if first != second {
                dest.write_char(' ')?;
                second.to_css(dest)?;
            }
            Ok(())
        }
    }
</%call>
</%def>

<%def name="four_sides_shorthand(name, sub_property_pattern,
                                 parser_function='crate::parser::Parse::parse',
                                 allow_quirks='No', **kwargs)">
    <% sub_properties=' '.join(sub_property_pattern % side for side in PHYSICAL_SIDES) %>
    <%call expr="self.shorthand(name, sub_properties=sub_properties, **kwargs)">
        #[allow(unused_imports)]
        use crate::parser::Parse;
        use crate::values::generics::rect::Rect;
        #[allow(unused_imports)]
        use crate::values::specified;

        fn parse_value<'i, 't>(
            context: &ParserContext,
            input: &mut Parser<'i, 't>,
        ) -> Result<Longhands, ParseError<'i>> {
            let rect = Rect::parse_with(context, input, |c, i| -> Result<
                crate::properties::longhands::${to_rust_ident(sub_property_pattern % "top")}::SpecifiedValue,
                ParseError<'i>
            > {
            % if allow_quirks != "No":
                ${parser_function}_quirky(c, i, specified::AllowQuirks::${allow_quirks})
            % else:
                ${parser_function}(c, i)
            % endif
            })?;
            Ok(expanded! {
                % for index, side in enumerate(["top", "right", "bottom", "left"]):
                    ${to_rust_ident(sub_property_pattern % side)}: rect.${index},
                % endfor
            })
        }

        impl<'a> ToCss for LonghandsToSerialize<'a> {
            fn to_css<W>(&self, dest: &mut CssWriter<W>) -> fmt::Result
            where
                W: Write,
            {
                let rect = Rect::new(
                    % for side in ["top", "right", "bottom", "left"]:
                    &self.${to_rust_ident(sub_property_pattern % side)},
                    % endfor
                );
                rect.to_css(dest)
            }
        }
    </%call>
</%def>
