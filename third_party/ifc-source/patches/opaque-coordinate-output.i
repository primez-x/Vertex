
// Preserve owned by-value coordinate returns without copying SwigValueWrapper.
%typemap(out, noblock=1)
IfcGeom::OpaqueCoordinate<3>,
IfcGeom::OpaqueCoordinate<4> {
  $result = SWIG_NewPointerObj(
      %new_copy($1, $1_ltype),
      $&descriptor,
      SWIG_POINTER_OWN | %newpointer_flags);
}
