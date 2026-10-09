# Vertex project formats v1 through v156

## Phase-aware physical-room wall edits (v156)

Native reader 156 and JSON/assets extraction 154 retain current-room wall
split/merge completion after phase bookkeeping changes, including wrapped,
retained, deleted and undone history. Earlier split v1/v2 and merge v1 intents
retain exact source-lineage replay.

Split intent version three has exactly the seven version-two fields plus
`physical_room_phase_completion:true`; its captured room/child allocation stays
mandatory. Merge intent version two has exactly the three version-one fields
plus the same true flag. New current command producers opt into these meanings;
the underlying historical completion APIs default to exact matching.

Each physical-room split/merge continuation receipt remains version one unless
the captured source differs only in `semantic_phases`. Version two then adds
exactly `current_source_descriptor` to that operation's existing fields.
`source_descriptor` remains the original descriptor for sequential history
chaining. Both descriptors are independently admitted against the preceding
clear geometry; selected source, context, physical inventory and holes must
remain unchanged. An unchanged descriptor, unknown fields, geometry/plane
change or any other lineage difference rejects this refresh meaning.
The strict physical partition/union and child correspondence then start from
the admitted current source and produce the destination descriptor in the same
command. Copying remaps identities in all three descriptors consistently.

Current, retained and wrapped command intents and version-two continuation
receipts set the reader floor. Extraction derives its floor from complete native
history; Undo cannot lower it. These are source contracts, with compilation and
runtime qualification pending under the current build/test pause.

## Framed passages and atomic overhead conversion (v155)

Native reader 155 and JSON/assets extraction 153 retain explicit passage
frames and the new atomic edit meanings throughout retained/undone history.
Opening assembly version four has exactly the original seven profile keys:
`version:4`, `kind:"opening"`, positive `frame_width_m`, positive `frame_depth_m`,
`panel_thickness_m:0`, `glazing_thickness_m:0`, and finite `inset_m`.
It constructs two jambs and a head with no sill, leaf, sash or glazing. It
accepts straight or fitted curved hosts. The unversioned entity kind parser
still leaves `"opening"` bare; historical cuts do not acquire a frame.

Hosted family-edit version two adds exactly `framed_passage_transition:true`
to the six version-one keys. Only the opening target family can carry this
flag. Its explicit assembly must be a passage or null for removal of framing.
The only new same-family conversion is bare cut to/from framed passage; editing
an existing frame still uses the profile-edit command. Host, dimensions,
quantity receipts, layers, metadata and unrelated owners remain authoritative.

Hosted profile-edit version two adds exactly
`materialize_default_door_assembly:true` to the ten version-one keys. It
requires an unchanged default door assembly and an overhead operation, null
dimensional edits and no clearing. Independent replay admits the actual
implicit retained door and complete source host before atomically materializing
its assembly. Ordinary single-field hints and same-value source representation
rules retain their prior meanings.

IFC exports a passage frame as a marked `IFCBUILDINGELEMENTPROXY` filling its
actual void. Import requires complete matching native profile/host/void
metadata, unique fill ownership, placement and regenerated mesh. Bare voids
remain unfilled. Passage schedules use generic assembly rows with frame fields,
without leaf, glazing or movement columns. These are source contracts;
compilation, output and runtime qualification remain open.

## Overhead tilt-up door operation (v154)

Native reader 154 and JSON/assets extraction 152 preserve overhead door poses,
including retained, deleted and undone opening entities. The optional
`door_operation` version three has exactly four fields: `version:3`,
`kind:"overhead_tilt_up"`, `side:"left"|"right"`, and finite
`opening_fraction` in [0,1]. It always decodes to a top horizontal hinge,
canonical 90-degree travel and zero sliding fraction. Earlier version one/two
grammars and arithmetic remain unchanged.

The opening requires an explicit door assembly and actual straight wall.
Its rigid panel and optional glazing rotate about the selected top thickness
edge: zero is closed and one is horizontal toward the selected wall normal.
Admission checks the requested full-panel envelope against the actual frame
and cut host. It does not certify continuous swept motion or a sectional track.
Plan presentation projects the actual posed solid so a raised panel cannot
disappear above the usual mid-height plan cut. No sideways swing arc is added.
The fallback linework API requires an explicit opening height; it does not
invent one from the opening width.

Schedules retain mechanism, top hinge and opening percentage. IFC uses an
explicit user-defined `OVERHEAD_TILT_UP` label and complete retained operation
metadata. Editing, history, saved phase replacements and reflection preserve
the same pose. Earlier readers refuse the new storage floor. These are source
contracts; compilation, physical output and runtime qualification remain open.

## Independent placed-component removal (v153)

Native reader 153 and JSON/assets extraction 151 retain coordinated demolition
inner seven in direct, drawing-wrapped, retained and undone history. Its phase
enclosure remains fifteen. The closed nine-field grammar matches inner five/six,
permits empty `ordinary_opening_ids`, and requires a nonnull ordinary child with
explicit qualified component keys. Child one keeps primitive roots; child two
still requires an actual roof and source-derived destinations. Historical
families are optional. Actual registered roots, carriers, placement hosts and
opening walls must agree on one real active saved choice; unregistered objects
remain ordinary without invented membership.

Only inner seven enables the independent placed-row policy. A sole active
proposed semantic opening may retain its exact physical body and same-registry
active existing/proposed wall while its explicitly selected component row is
removed. An active nonrequired original catalog may retain its envelope and
membership. Ordinary/proposed carriers retain their existing removal admission.
The final raw carrier must equal the original with only admitted rows filtered,
including unchanged definitions, materials and surviving order. The opening and
wall bodies and their retained phase membership remain exact. The protected-row
exception is derived from the actual selection producer; it cannot be supplied
by an alias, historical closure or a fabricated reduced source.

Controller Delete/Cut now reaches the typed operation for component-only
selections and includes actual opening walls in Site admission. Source,
selection and pending-placement guards remain through application; Cut content
publishes afterward. The separate roof, wall and placed policies reach both
analytical admission and replay. Older inner versions and default callers keep
their previous meaning and reader floors. No compilation or runtime acceptance
is claimed.

## Complete mixed-wall hosted catalog consequences (v152)

Native reader 152 and JSON/assets extraction 150 retain wall-demolition
authoring inner three under the existing exclusive phase-authoring dialect
sixteen. Inner three has exactly nine fields: the six inner-one fields plus
`ordinary_wall_ids`, `wall_additional_identities` and
`complete_hosted_catalog_consequences:true`. Empty ordinary-wall roots require
empty wall destinations; nonempty roots retain inner-two's source-derived join
slots and budgets. Historical children remain restricted to their original
demolition dialects, including coordinated inner one through three.

This opt-in completes ordinary proposed wall, roof and independent opening
consequences while original walls are demolished in the same saved alternative.
Actual roots, qualified rows and hosts authenticate before historical/native
leaves. Only an active, nonrequired baseline catalog in the actual proposed
host's saved registry may retain its carrier while admitted placed rows retire.
Selected physical walls must have sole active proposed ownership. Baseline,
foreign, required, inactive and protected physical owners remain preserved.
An independently admitted proposed opening may retire its placed row on an
unchanged original wall; the actual opening must be physically absent and the
original wall remain byte-exact. That exception persists through room review.

Physical wall replay and join-inference preflight share the opt-in qualified
catalog policy with the component producer. The selection façade forwards the
separate wall policy through both component preflight and replay; roof-only
and non-wall host admission remains independent. Admission preserves complete source
work budgets, aliases and inactive ownership. Retained catalog envelopes,
definitions, materials, raw survivor order and membership are proved against
the original carrier. Compound consequences use complete source-based
composition, followed by the existing explicit detached-room review. Controller
Delete/Cut retain source, selection, pending-placement and Site authority, with
Cut publication only after application. Fresh-ID admission reserves all new
proof vocabulary and retained/undone destinations. Defaults and older inner
one/two meanings are unchanged; no build or runtime acceptance is claimed.

## Complete proposed-roof catalog consequences (v151)

Native reader 151 and JSON/assets extraction 149 retain coordinated demolition
inner six in direct, drawing-wrapped, retained and undone history. The exclusive
phase-authoring enclosure remains fifteen. Inner six has exactly inner five's
nine fields, permits an empty `ordinary_opening_ids` array, and requires a closed
ordinary-removal child two with an actual selected roof and actual join slots.
Historical families remain optional; without them, the actual saved registry
and alternative are derived from actual opening/host and roof membership.
Existing aggregate, identity, source-binding and destination budgets remain.

Only inner six opts into complete proposed-roof catalog consequences. The roof
must be exclusively proposed in the catalog's actual active saved registry;
the baseline carrier stays present with its original membership. Retirement
filters only admitted qualified roof-hosted rows from the original raw carrier,
preserving its envelope, definitions, materials, unrelated rows and survivor
order. Baseline roofs, required/inactive/foreign carriers and protected owners
remain refused. Compound roof/primitive/component removal composes complete
actual-source consequences rather than dropping either catalog edit.

Delete/Cut capture the complete source and selected roof slots, route through
this typed operation, and retain existing pending-placement and Site fences.
Site admission includes affected roof joins and qualified component aliases.
Document replay preserves source/history authority and reserves every fresh
destination across retained and undone operations. Older coordinated dialects,
historical wall children and default roof façades keep their original semantics
and reader floors. No build or runtime acceptance is claimed.

## Independent ordinary openings in coordinated demolition (v150)

Native reader 150 and JSON/assets extraction 148 retain coordinated demolition
inner five through direct, drawing-wrapped, retained and undone history. The
phase-authoring dialect remains fifteen. Inner five has exactly the eight
inner-four fields plus `ordinary_opening_ids`: one to 1,000 ascending unique
actual semantic opening identities, disjoint from historical and ordinary roots.
The aggregate historical/ordinary root, component and opening count is bounded
at 4,096. With no historical family, the saved choice is derived from actual
opening or inherited proposed-wall membership; at least one actual active
registry is required. Unregistered owners remain ordinary without fabricated
membership. Inner versions one through four keep
their exact grammars, meanings and reader floors.

Every independent opening and actual wall host must pass source ownership and
saved-choice admission before native replay. Baseline opening retirement and
required, foreign, inactive or protected-other-alternative owners are refused.
The actual opening-removal producer independently derives its complete map,
including qualified catalog row and known reference retirement. Its candidate
enters composition before ordinary component selection collapse, so original
qualified source admission still precedes any covered-row collapse. All existing
raw envelope, registry order, baseline body, alias, metadata and saved-alternative
protections remain. Complete composition may contain seven admitted candidates;
historical retirement-only callers retain their existing five/six-map bounds.

Pure door/window selections and broader architectural/drawing selections use
this same typed operation when original and ordinary/proposed openings are mixed.
Source/selection and pending-placement fences run before preparation and before
application; Cut publishes captured content only after application. Site
authority includes actual opening and wall-host IDs. The new opening proof token
is reserved only by inner five. Historical wall-authoring dialect sixteen remains
restricted to historical coordinated children one through three, preventing new
semantics from borrowing its older reader floor.

Only this typed new lane can retire actual rows hosted on an admitted proposed
opening from an unchanged baseline catalog carrier, including a proposed cut
on an unchanged original wall. Original catalog definitions, materials, raw
survivor order, all other rows and membership remain intact. Original opening
rows and protected hosts cannot borrow this exception; unregistered openings
inheriting a shared baseline wall still require ownership resolution. Historical
opening callers and inner-four catalog protections remain unchanged. Compilation
and runtime remain unverified under the source-only instruction.

## Complete coordinated architectural demolition (v149)

Native reader 149 and JSON/assets extraction 147 retain coordinated demolition
inner four under phase-authoring dialect fifteen, including direct commands,
independent drawing completion forty-two, retained and undone history. Earlier
inner versions retain their exact meanings and reader floors.

The closed inner-four record has exactly `version`, `opening_authoring`,
`roof_authoring`, `slab_authoring`, `structural_authoring`, `stair_authoring`,
`ordinary_removal` and `complete_hosted_catalog_consequences:true`. Historical
family slots contain complete canonical demolition envelopes bound to the same
actual Snapshot and saved choice. At least two families are required when
ordinary removal is null; otherwise at least one. The ordinary child retains
its closed version-one or version-two grammar, including explicit roof split
destinations for version two.

Each family independently replays the original entity map. Explicit ordinary
roots and qualified component rows must pass actual source ownership checks
before consequences covered by a historical leaf can collapse. Composition
admits exact retained catalog rows and authenticated fresh suffixes while
preserving catalog envelopes, definitions, materials and opaque fields.
Baseline physical owners, required/foreign/inactive evidence, other alternatives,
surviving aliases and raw registry metadata remain protected. The registry
adapter changes only known ID lists, preserves their retained raw order, and
checks unchanged typed meaning. Known identical reference retirement composes
once; conflicting physical or join consequences refuse.

Document admission reserves resulting and declared destinations against the
complete source and retained history, assets, component aliases and this new
proof vocabulary. Delete/Cut assemble one complete command without partial
family Document previews or UI-only consequence pruning. Mixed selections may
include independently selected drawing owners, labels and symbols. Selection,
pending-placement and Site publication fences stay attached to that full
operation; Cut updates its clipboard only after application. Physical walls and
independent ordinary openings retain their dedicated completion workflows.
Compilation and runtime remain unverified under the source-only instruction.

## Mixed original and ordinary/proposed wall removal (v148)

Native reader 148 and JSON/assets extraction 146 retain inner-two complete wall
demolition, including wrapped, retained and undone history. The enclosing
phase-authoring dialect remains sixteen. Inner one keeps its exact six fields
and native 147 / extraction 145 meaning. Inner two adds exactly
`ordinary_wall_ids` and `wall_additional_identities`; it requires nonempty,
ascending unique ordinary walls, disjoint baseline roots, at most 128 combined
wall roots and at most 4,096 unique declared split-join destinations.

The baseline lane retains the original walls and hosted records while changing
the actual saved alternative. The ordinary lane independently derives complete
physical removal from the same original map, including semantic openings, hosted
catalog rows, joins and known references. Shared baseline, required, inactive and
foreign saved owners cannot enter the ordinary lane. Actual qualified selection
admission precedes collapse of already covered opening or component consequences.
Exact known reference retirements compose once; physical or join conflicts refuse.

Snapshot inspection and current/retained Document source admission reserve
ordinary split identities before native inference. Retained replay uses its
explicit preceding history prefix; current preparation also reserves retained
undone records, assets, metadata and component aliases. Final complete admission
checks the combined wall, roof and room destinations. Only inner two reserves
the newly introduced semantic proof vocabulary; inner-one retained identities
keep their historical meaning. The complete stage supplies the existing explicit phase-room review;
no intermediate Snapshot or family preview becomes authority.

Wall-only and broader Delete/Cut selections use this path when originals and
ordinary/proposed walls are selected together. Site authority includes both
wall inventories and changed component aliases. Independent drawing removal can
complete the same operation, and Cut publishes its clipboard after application.
Compilation and runtime remain unverified under the source-only instruction.

## Complete baseline wall demolition authoring (v147)

Native reader 147 and JSON/assets extraction 145 retain phase-authoring dialect
sixteen under the existing command envelope thirty-four, including retained and
undone history and independent drawing completion forty-two. Its exact nine
fields are the eight common phase-authoring fields plus `wall_demolition`.
Other phase-authoring operation fields and geometry/relationship authority are
excluded. Historical dialects retain their meanings and reader floors.

The closed inner-one record has exactly `version`, `wall_demolition`,
`other_authoring`, `ordinary`, `opening_ids` and `room_review_intent`. Its wall
leaf names actual baseline walls, their registry and the actual saved
alternative. `other_authoring` is null or one same-source canonical historical
demolition envelope for openings, roofs, slabs, structural objects or stairs,
including coordinated demolition. It cannot recursively enclose dialect
sixteen or supply a transform/replacement unrelated to demolition. `ordinary`
contains exactly `object_ids`, qualified `components` and explicit
`roof_additional_identities`. Independent ordinary openings use `opening_ids`.
Ordinary wall removal requires its separate wall-authoring path.

Every lane independently replays the complete actual source before composition.
Only known source-row removals, declared fresh destinations and exact hosted
instance suffixes compose. Restoring the source instance inventory must recover
the original catalog envelope, definitions, materials and opaque fields.
Physical owners, shared baselines, other saved alternatives, inactive evidence
and surviving component aliases remain protected. An explicit selection already
covered by an authenticated host or historical retirement consequence collapses
only after its actual source ownership has been admitted.

Room inspection uses the complete analytical stage while keeping the original
Snapshot, history, save state and saved-choice bindings. The stage registry's
identity upsert binds room geometry and membership without fabricating a
Snapshot. Every required retained/fresh room and dependent reference decision
remains explicit; incomplete room decisions cannot publish. Document admission
reserves declared and resulting destinations against retained history, assets,
component aliases, nested identities and entity envelope names, including
omitted historical roof-copy slots.

Delete/Cut retain the captured full selection and Site publication fence through
room review and application. Independent drawing selection may complete the
same command afterward; Cut publishes its captured clipboard only after the
complete operation applies. Compilation and runtime remain unverified under
the source-only instruction.

## Independent drawing removal around active-design authoring (v146)

Native reader 146 and JSON/assets extraction 144 retain separate additive
command envelope forty-two, including retained and undone history. Its exact
seven fields match forty-one, but `proof` must be one canonical, unnested
active-design command thirty-four with the same revision and message.
Forty-one retains its complete wall/room meaning and cannot enclose direct
active-design authoring.

The Document authenticates the original phase source/history/save bindings,
reconstructs that complete stage, and then replays the closed independent
drawing selection against the actual source and admitted stage. Active-design
policy, inactive ownership, original assets, metadata and history reservations
remain enforced. Every resulting or declared phase destination is reserved
against retained assets, component aliases, entity envelopes and the combined
proof vocabulary. No raw payload gains phase geometry authority.

The desktop uses this completion when baseline doors/windows and independent
drawing items are deleted or cut together. One command retains the original
opening bodies in the baseline and removes them from the active design, while
removing only the explicitly selected drawing items. Publication retains the
full captured selection and Site fence; Cut updates the clipboard afterward.
Compilation and runtime remain unverified under the source-only instruction.

## Independent drawing removal around wall/room review (v145)

Native reader 145 and JSON/assets extraction 143 retain additive command
envelope forty-one, including undone history. It has exactly `version`, `kind`,
`expected_revision`, `message`, `independent_drawing_removal_completion`,
`independent_drawing_removal_intent` and `proof`. The marker is explicitly true;
the unnested child is one exact preceding complete wall/room command with the
same revision and message. Older envelopes keep their existing meanings.

The closed version-one intent contains `version`, ascending unique `owner_ids`
and ordered `annotations`. Each annotation has its actual `owner_id`, explicit
`kind` (`label` or `symbol`) and local `child_id`. Render aliases, bare ambiguous
children and annotation containers cannot grant removal authority. Independent
area boundaries, supported measured strokes and placed dimensions use actual
owners; physical room/wall lineage remains under its dedicated review.

Analytical actual-source selection, ownership, opaque-reference and constraint
admission precedes geometry replay. The Document independently reconstructs the
exact original wall/room stage, then the drawing removal. Supported dimensions,
constraints, deductions, annotation overrides and presentation memberships are
retired with their owners. Identical already-admitted reference retirement can
collapse; surviving raw rows, computed component aliases, inactive ownership,
assets and metadata remain intact. The final complete Document state is checked
before one event is applied. Cut writes the clipboard only after application.

Wall-free opening/drawing combinations and wall removals without retained room
review use the existing asset-free raw version-one command after complete source
reconstruction and preview; they introduce no new reader floor. The desktop
retains the complete original selection and Site source fence across room
dialogs. The command decoder ceiling now includes existing children thirty-five
through forty as well as the new outer envelope. Compilation, runtime and
save/reopen remain unverified under the source-only instruction. Affected opaque
references and protected owners still require additional authoring support
rather than inferred changes.

When deleting the last explicit source from a supported sheet/view model
version one through five, cleanup copies the complete raw entity and promotes
its model to the existing version-six reader representation before pruning.
Only missing defaults already defined by that reader are added; all interpreted
view, overlay, sheet, schedule and order semantics must remain equal. The emptied
view retains `restrict_to_objects: true`. A pre-v4 raw `sheet_order` conflicting
with its legacy interpreted order refuses. Unsupported schemas cannot use this
migration. This adds no new model dialect or reader floor.

Mixed shared-baseline wall and architectural demolition composes independently
source-bound, retained-body candidates into one raw active-registry update.
Physical bodies, catalog rows, other alternatives and saved metadata remain
exact. Existing explicit phase-room review completes the registry stage before
publication; independent drawing removal uses the existing completion above.
Ordinary/proposed erasure, roof cohorts and fresh destination cohorts require their own
complete authoring lane and cannot borrow this registry-only authority.

## Ordinary stair attachment edits in existing raw commands

Ordinary stair edits with explicit railing rehost/removal decisions use the
existing asset-free raw version-one command. All retained entities use their
existing supported stair, railing, catalog and presentation dialects; this
producer introduces no additional reader floor. The source-derived compound
profile/placement replay and complete rail retirement run before the single
command is created. The actual snapshot supplies retained child lifetime,
constraint policy, assets and source authority. Protected phase owners remain
under their existing authoring paths.

The complete command and candidate pass exact real Document preview before
publication; Undo retains the whole edit. Compilation, runtime and save/reopen
remain unverified under the source-only instruction.

## Mixed wall and semantic opening removal with room review (v144)

Native reader 144 and JSON/assets extraction 142 retain child forty under a
single room review or the existing batch twenty-seven, including retained and
undone history. The closed `mixed_wall_opening_deletion` envelope has exactly
`version`, `kind`, `expected_revision`, `message`, `intent` and `proof`.
Its six-field intent has `wall_ids`, `opening_ids`,
`wall_additional_identities`, `other_object_ids`, `components` and
`roof_additional_identities`. At least one actual wall and one actual semantic
opening are required; other architectural owners and qualified catalog rows
are optional. The child is the exact bounded, asset-free raw version-one
command. Declared wall and opening roots must be erased, and declared fresh
destinations must be upserted.

Openings hosted by selected walls collapse into those walls' complete
consequences. Independently selected openings use their own complete-source
leaf. Optional roof, horizontal, stair, railing, column, beam and component
leaves read the same immutable actual source. Identical supported reference
erasures and actual catalog/presentation row omissions can be shared once;
conflicting edits, required relationships and duplicate physical-owner changes
are refused. Historical composers keep their default overlap policy.

Fresh wall and roof destinations share actual snapshot, retained history,
asset and presentation reservations before geometry production. The actual
snapshot/history supplies the explicit active-constraint policy. Independent
replay must reproduce the entire raw command and candidate, preserve surviving
aliases, inactive ownership and room lineage, and admit the result through the
real document preview. Reviewed room consequences apply in the same command;
Cut publishes its clipboard only after successful application. Historical
children keep their prior meanings. Compilation and runtime remain unverified
under the source-only instruction.

## Reviewed proposed railing attachments in stair replacement (v143)

Native reader 143 and JSON/assets extraction 141 retain stair-replacement
child five in direct, coordinated and wrapped authoring, including undone
history. It adds `dependency_rail_ids` and `dependency_dispositions` to the
existing profile or compound replacement fields. `preserved_inactive_rail_ids`
is present even when empty. The dependency witness is a nonempty, ascending
inventory of actual affected proposed rail owners, with one canonical decision
per owner in the same order. Transform-only authoring cannot borrow this lane.

Rehost decisions choose actual resulting flights or landings, including their
incident-flight pair. Retirement removes only actual active-only proposed
railings in the saved alternative, plus their admitted hosted catalog rows and
known references. Original baseline stairs, inactive attachments, other
alternatives and catalog definitions remain retained. Complete-source replay
checks the decision witness and derives every consequence; supplied entity maps
or inferred geometry grant no authority.

Fresh destinations remain reserved against the original source and its retired
aliases, retained history and assets. Coordinated candidates preserve exact raw
catalog survivors and refuse overlapping changes. The editor's decisions bind
the complete captured source before one command applies. Older children one
through four retain their original meanings. Compilation and runtime remain
unverified under the source-only instruction.

## Mixed wall removal with manufactured opening hosts (v142)

Native reader 142 and JSON/assets extraction 140 retain mixed-wall deletion
child thirty-nine in single or batch room review. Its seven fields are the
closed thirty-seven fields plus exactly `complete_opening_hosted_removal: true`.
The existing five-field intent remains unchanged. This explicit lane admits
actual manufactured opening hosts for qualified catalog components and removes
the rows hosted on openings retired with selected walls.

Every leaf reads the same complete source. The raw child and whole candidate
must equal independent replay, with exact surviving aliases, protected phase
ownership and retained room lineage. Analytical admission and reserved proof
names precede native leaves; fresh wall and roof destinations share complete
snapshot/history reservation. Historical thirty-seven retains its original
admission and is preferred only when the entire command independently qualifies.
Independent and root source reviews are complete; compilation and runtime remain
unverified.

## Wall removal with opening-hosted components (v141)

Native reader 141 and JSON/assets extraction 139 retain physical-wall deletion
child thirty-eight in single or batch room review. Its ten fields are the
closed thirty-six fields plus exactly `complete_opening_hosted_removal: true`.
Actual retired qualified catalog rows on the removed semantic openings justify
the new lane. The flag grants no arbitrary retirement authority; all source,
raw-command and candidate consequences must independently match replay.

The opening body uses the actual wall, active sibling cuts, saved elevation,
family and door operation. Legacy copies retain their world placement. Bare
cuts cannot supply a manufactured legacy-copy body. Catalog definitions, raw
survivors, aliases, protected phase ownership and room lineage remain intact.
Analytical opening/component admission shares the wall/join work reservation
before native factories. No affected join is required when the new opening
component consequences are present.

Historical raw, thirty-one, thirty-five, thirty-six and mixed thirty-seven
proofs keep their previous meanings. Allowing the new option without actual
opening-hosted rows retains the historical producer and proof preference.
Independent and root integrated source reviews are complete. Compilation and
runtime remain unverified.

## Mixed wall and architectural removal with room review (v140)

Native reader 140 and JSON/assets extraction 138 retain the explicit
`mixed_wall_deletion` child thirty-seven under a single room review or the
existing batch twenty-seven. Its six fields are `version`, `kind`,
`expected_revision`, `message`, `intent` and `proof`. The intent has exactly
`wall_ids`, `wall_additional_identities`, `other_object_ids`, `components` and
`roof_additional_identities`. Qualified component selections are catalog/local
identity pairs; aliases do not grant removal authority.

Every producer independently reads the same complete actual source. Selected
walls retire their attached openings and supported components. Roofs, horizontal
assemblies, stairs, railings, columns, beams and qualified catalog components
compose into one candidate. Fresh split-wall and roof destinations share source,
history, asset and presentation-namespace reservations. The raw child and whole
candidate must equal independent replay; room identities and lineage remain
intact until explicit room decisions are attached. Cut publishes only after the
single completed command applies.

Historical raw, thirty-one, thirty-five and thirty-six proofs retain their
original meanings. Independent opening, room, drawing and annotation roots do
not acquire authority through thirty-seven. The shared conservative source and
native-work limits can refuse large unrelated inventories; this remains a
capacity gap. Independent and root integrated source reviews are complete.
Compilation and runtime remain unverified.

## Complete wall-join removal with room review (v139)

Native reader 139 and JSON/assets extraction 137 retain physical-wall deletion
proof thirty-six under single room-review dialect thirty-six or batch
twenty-seven. The closed nine fields extend proof thirty-five with exactly
`complete_join_removal: true` and `additional_join_identities`, a source-join
map of captured destination IDs in derived component order. Original joins are
admitted against actual walls and openings before removal. Surviving connected
multiwall groups keep the original join ID for the first group and use the exact
declared fresh IDs for later groups; isolated walls retain no join owner.

Replay independently derives the complete wall, opening, component, join,
membership and known-reference consequences from the original source. The raw
child must equal the whole derived command. Rooms and physical lineage remain
unchanged until the same context/plane review accepts their consequences. Shared
baseline joins, required/inactive owners, foreign alternatives and opaque
affected references remain protected. Unrelated catalog definitions, survivors,
aliases and raw phase roster order remain exact.

Declared join destinations are reserved against actual source, retained history,
assets, local/opaque metadata and component presentation aliases. Undone command
intent also reserves its names. Production and Document replay share a bounded
read-only reservation engine; retained replay considers only preceding records.
Historical raw singleton, grouped thirty-one and hosted-complete thirty-five
retain their original producer meanings. Projects without thirty-six retain
their previous required reader floor. Independent and root source integration
reviews are complete; compilation and runtime remain unverified.

## Complete hosted wall removal with room review (v138)

Native reader 138 and JSON/assets extraction 136 retain physical-wall deletion
proof thirty-five under single room-review dialect thirty-five or the existing
batch twenty-seven. Its closed fields retain the historical grouped proof's
version, kind, revision, message, sorted source wall IDs and canonical asset-free
raw child, plus exactly `complete_hosted_removal: true`. One through 128 actual
walls are permitted. The complete typed source producer independently derives
all opening, hosted-component, reference and presentation consequences; the raw
child must equal that full derivation. No arbitrary deletion authority follows
from the marker.

The capture encoder retains historical raw singleton or grouped thirty-one when
the entire command equals the old producer. New consequences require independent
complete-source qualification before selecting thirty-five. Historical retained
proof replay continues to use the old producer. Complete removal protects actual
inactive/shared-alternative ownership, raw catalog definitions and surviving
qualified aliases. Room owners and physical lineage remain unchanged until the
same context/plane review accepts their consequences.

Current, retained and wrapped commands retain their respective floors; the
source reader accepts the new single/batch room proof without changing older
dialects. Source implementation and integration review are in progress.
Compilation and runtime remain unverified.

## Coordinated ordinary roof removal (v137)

Native reader 137 and JSON/assets extraction 135 retain coordinated demolition
inner three under exclusive active-design authoring fifteen. The closed seven
fields retain the five full historical family envelopes and `ordinary_removal`.
At least one historical family and an actual ordinary roof are required.
Ordinary child two has exactly `version`, `object_ids`, `components` and
`roof_additional_identities`; the latter maps actual join or bound-overlay slots
to bounded nonempty fresh identity arrays. Actual roof contact and presentation
derivation must independently reproduce every supplied slot. No entity-map,
geometry or arbitrary retirement payload is accepted.

Roof and primitive/component lanes independently admit the same complete actual
source. Hosted retirement applies only to physically removed roofs; source
retention preserves original hosted rows. Shared catalogs retain raw definitions,
survivors, row order and aliases. Registered affected roofs, joins, component
carriers/hosts and actual retained-roof registry changes must match the same
historical saved choice; other alternatives remain protected. Source shared
baseline joins cannot mutate even in saved baseline. Composition merges only
known roster/presentation rows and exact catalog retirement consequences.

Declared join/overlay destinations remain reserved across Undo, abandoned
history, wrapped proofs and retained assets. Current and retained commands retain
the new reader floor. Inner one/two and ordinary child one keep their original
wire and replay meanings. Roof source review approved; mixed integration review
approved after closing generic retention and enrollment findings. Root integrated
source review is complete. Compilation and runtime remain unverified.

## Stair replacement with retained inactive topology (v136)

Native reader 136 and JSON/assets extraction 134 retain stair replacement child
four under direct active-design authoring twelve or coordinated authoring
fourteen. The closed nine fields retain the common registry, alternative and
qualified destination maps; exactly one existing mathematical lane (`edits`,
`transforms` or `compound_edits`) is present. The additional
`preserved_inactive_rail_ids` is a nonempty, ascending, unique actual entity-ID
witness. It must match independently derived inactive attachments to the
actually changed baseline stairs. It grants no removal, supplied-source or raw
entity authority.

Replay creates only a bounded additive staging cohort derived from the actual
source. Originals and their inactive attachments remain valid and unchanged
while the copied active cohort receives the captured typed mathematical edit.
Only codec-owned temporary identity slots reverse into edited descriptors;
opaque envelopes and entered quantities still require their existing codecs.
The final proposed cohort retains globally fresh topology, actual hosted rows,
valid active rail references and complete Document/history/assets admission.
Legacy children one through three keep their original replay paths. Current,
retained and wrapped proofs preserve the child-four floor. Intermediate native
silhouettes and bound dimensions resolve qualified owners in the full admitted
additive map; reversed descriptors supply only host and placement bookkeeping.
Independent and integrated source review approved; compilation and runtime
remain unverified.

## Mixed baseline and ordinary removal (v135)

Native reader 135 and JSON/assets extraction 133 retain active-design authoring
fifteen with coordinated demolition inner two. It retains the five historical
family fields and adds exactly `ordinary_removal`. At least one historical
family and a nonempty ordinary selection are required. The ordinary child has
exactly `version` (one), `object_ids` and `components`; each component has exactly
`catalog_id` and `instance_id`. Object IDs and qualified pairs are ascending,
unique supported ASCII identities, with at most 1,000 aggregate selections.
No entity, geometry or arbitrary deletion payload is accepted.

Every lane independently replays the same actual source. Actual ordinary or
proposed stairs, railings, horizontal assemblies, columns/beams and qualified
component rows use their complete removal producer. Registered removed owners,
component carriers and hosts must belong to the historical saved registry and
alternative; unregistered ordinary owners remain eligible. Shared baseline
physical owners remain exact. A shared catalog can change only through exact
raw row retirement, retaining its envelope, definitions, surviving rows and
order. Surviving component aliases remain unchanged. Other alternatives and
inactive annotations retain their raw ownership and row order.

Delete/Cut partition the captured selection automatically. A baseline leaf's
actual dependent retirement or host demolition covers a separately selected
dependent once. The complete command retains source/history/assets authority;
Cut publishes clipboard content only after success. Inner one keeps its
historical reader 134 meaning. Current, retained and wrapped proofs retain
their respective floors. Independent and integrated source review approved;
compilation and runtime remain unverified.

## Ordinary architectural and component removal

Ordinary stair/railing, column/beam and horizontal removal captures actual
attached rails, hosted catalog rows, dimensions and known saved presentations
in one complete candidate. Explicit embedded component selections use exact
catalog/instance pairs through the same producer, including ordinary wall/roof
hosts. Catalog definitions, unaffected rows and surviving render aliases remain
retained; unsupported affected references refuse before publication.

Baseline-only registries without alternatives may retire selected baseline
members and their registry roster together. Active proposed owners may retire
only when no other alternative uses them. Shared/inactive baseline and other
alternative ownership remains protected. A shared catalog carrier can lose
only actual rows hosted by removable owners without changing its own registry.
Unhosted rows derive their authority from the actual carrier.

These commands persist their complete resulting entity maps under existing
entity dialects without a new reader floor. Clipboard preparation redirects
known temporary component presentation and dimension references before detached
source admission. Local material, topology and dimension child references are
qualified by their actual catalog or physical owner. Alias preservation is
checked again after drawing cleanup and final command augmentation. The original
source remains immutable. Independent and integrated source review approved;
compilation and runtime remain unverified.

## Mixed architectural demolition (v134)

Native reader 134 and JSON/assets extraction 132 retain active-design authoring
fifteen with closed coordinated demolition inner one. Its exact keys are
`version`, `opening_authoring`, `roof_authoring`, `slab_authoring`,
`structural_authoring` and `stair_authoring`. Absent children are null; at least
two families are present. Each child is its full historical source-bound
demolition envelope and names the same saved registry and alternative. No
geometry, relationship, replacement-body or nested coordination authority may
accompany a demolition child.

Each family independently replays the complete actual source. Composition
retains baseline physical owners, merges disjoint codec-known registry and
presentation rows, retires only actual proposed dependents, and preserves
surviving hosted row bytes and render aliases. An emptied restricted view stays
restricted. Fresh roof/join destinations remain subject to Document lifetime
reservation. Delete and Cut publish one complete undoable command; Cut prepares
the clipboard before publication. Current, retained and wrapped proofs retain
the format floor. Independent and integrated source review approved;
compilation and runtime remain unverified.

## Combined stair profile and placement edits (v133)

Native reader 133 and JSON/assets extraction 131 retain stair replacement child
three under direct active-design authoring twelve or coordinated authoring
fourteen. The same eight closed keys replace the operations array with
`compound_edits`. Each closed compound intent one contains `version`,
`profile_edit` and `placement_edit`, naming the same actual owner.

The complete edited cohort is admitted against the actual source before
separating profile from pose. Profile replay retains source base, orientation
and coordinate receipts; vertical bindings and nonplacement entered quantities
remain profile authority. Rigid yaw and XYZ translation then anchor at the
resolved intermediate profile. Active attached rails and hosted rows follow
once. Final coordinate receipts use the existing typed transform policy.
Ordinary commands persist resulting entity data without this internal proof.

Alternative replay preserves original baseline owners and catalogs, reserves
fresh owner-qualified topology/components/overlays, and rehosts active proposed
dependents. Bound dimensions use the intermediate profile silhouette before
placement; raw styles and opaque presentation fields remain retained. Existing
child one/two meanings remain unchanged. Independent and integrated source
review approved; compilation and runtime remain unverified.

## Coordinated stair and architectural transforms (v132)

Native reader 132 and JSON/assets extraction 130 retain active-design authoring
fourteen with closed coordinated inner four. Its exact keys are `version`,
`wall_authoring`, `roof_replacement`, `slab_replacement`,
`structural_replacement`, `stair_replacement`, `ordinary_roof_edits`,
`ordinary_slab_geometry`, `ordinary_structural_edits` and
`ordinary_stair_transforms`. At least two families are required. Each family
replays independently against the same complete actual source; the stair
replacement and ordinary transform list are exclusive. Ordinary stair rows
cannot borrow inactive or shared-baseline authority.

Composition merges disjoint actual catalog placement changes and qualified
presentation additions, while preserving retained baselines and other
alternatives. Fresh entity, child, hosted, overlay and computed names remain
subject to Document history/assets reservation. Wall and room review remains
mandatory. Current, retained and wrapped proofs keep the floor; a nested stair
child three raises it to 133. Earlier envelopes keep their meanings. Independent
and integrated source review approved; compilation and runtime are unverified.

## Mixed drawing and architectural copies

Independent selection copy composes source-derived drawing and physical family
candidates against the same complete source. Architectural children, hosted
components and saved presentations retain qualified fresh identities. Drawing
boundaries, rooms, annotation children and reference dependencies keep their
own complete graph-copy authority. Carrier-local appearance rows may target
their actual label or symbol children without creating physical host authority.

Separately selected embedded components and callouts complete before command
capture is sealed; captured scope enrollment must reproduce that exact command
plus its derived registrations. This operation saves resulting entity dialects
without a new project-format floor. Independent and integrated source review
approved; compilation and runtime remain unverified.

## Source-derived independent stair and railing copies

The internal closed clone intent one has `version`, `transforms`, `identities`,
`child_identities`, `hosted_instance_identities` and `overlay_identities`.
Captured transforms replay against the complete actual source. Physical,
owner-qualified topology, catalog-local hosted rows and view-local overlays
require complete exact mapping inventories. Active attached rails accompany the
selected host. Fresh private catalogs contain only actual transformed selected
rows, retaining their definitions and opaque metadata. Original physical owners,
catalogs, registries, levels and organization remain exact.

Saved view and object annotation arrays append corresponding copied records.
Physical and legacy host-derived geometry uses source coordinates; expanded
type-owned components use world coordinates. The shared effective section plane
governs overlay projection. Bound dimension axis/line placement is derived from
the source and copied silhouettes; other style and opaque fields stay exact.
Mixed family composition retains each leaf's aliases and appended records.
The caller reserves retained history/assets and computed names before Document
admission and explicit scope enrollment. Ordinary entity commands persist the
resulting data, not this internal clone proof; existing entity/catalog dialect
floors govern the result. Independent integrated source review approved.
Compilation and runtime qualification remain pending.

## Entered stair and railing transform quantities (v131)

Native reader 131 and JSON/assets extraction 129 retain transform intent two
inside stair replacement child two. Its exact keys are `version`, `object_id`,
`transform` and `quantity_entries`. The complete entered-input map is validated
against actual-source transformed profiles. Transform intent one keeps its
original three keys and source-derived receipt retention semantics.

Numeric placement edits validate the complete original edit before separating
pose from profile. Classification resets only coordinate receipts alongside
the original position; other quantity and opaque metadata stays protected. The
captured transform retains the validated complete entered map. Unchanged raw
inherited coordinate receipts remain exact, including opaque source evidence.
New input for an unchanged coordinate must match its metres exactly. A changed
coordinate may restore only floating roundoff within `1e-12 * max(1, abs(value))`
to its exact entered metres; other geometry cannot be supplied through receipts.
Typed profile admission still validates the full receipt map and metadata.
Ordinary entity commands do not persist this transform proof and acquire no
new floor. Current/history and wrapped alternative proofs retain reader 131.
Independent integrated source review approved; compilation and runtime
qualification remain pending.

## Stair demolition with proposed railing retirement (v130)

Native reader 130 and JSON/assets extraction 128 retain active-design authoring
thirteen. Its exclusive `stair_demolition_retirement` child one has exactly
`version`, `registry_id`, `alternative_id`, `selected_object_ids` and
`retired_proposed_rail_ids`. Replay independently discovers the complete active
proposed railing closure; supplied identities cannot authorize other deletion.

Retirement removes those rail owners from both registry membership and the
active alternative's proposal list. It removes only their actual hosted catalog
rows and codec-known object/view/annotation references, retaining empty catalog
owners, definitions, other alternatives and organization/level records. Existing
baseline demolition then applies to the retained selected stair and baseline
rail closure. Unsupported affected references or changed surviving component
aliases refuse. Protected inactive annotation rows retain raw order and values
even when earlier active rows are removed. Historical ordinary annotation guard
semantics remain unchanged. Window selection may include the exact derived
proposed rail closure; the canonical selected roots remain baseline-only.
Blocking diagnostics are capped at 128 to bound refusal work on large sources.
Independent integrated source review approved. Runtime qualification is pending.

## Stair and railing alternative transforms (v129)

Native reader 129 and JSON/assets extraction 127 retain stair replacement child
two under active-design authoring twelve. Its exact keys match child one except
that `transforms` replaces `edits`. Each captured typed transform names an actual
owner and its complete pivot, XYZ offset, rotation, uniform scale and reflection
operator. Profile and transform authorities are exclusive; child one retains
its earlier meaning.

Actual-source transform replay coordinates active attached rails and hosted
parts before proposing baseline replacements. Baseline physical/catalog owners
and inactive dependents stay exact. Type-owned hosted profiles use the actual
Site world frame; legacy host-derived profiles use the source frame. Private
catalogs copy the actual transformed selected rows. Physical and legacy hosted
saved-view presentations use source coordinates; type-owned profiles use their
actual world coordinates. Saved unbound overlays use the shared effective view
origin, including section cut displacement and retained built-in interpretation,
and axes, while bound dimension placement derives from the
actual source and proposed geometry. Unrepresentable bound axes/depth coupling
and conflicting baseline catalog schema upgrades refuse explicitly.
Current/history and wrapped proofs retain the reader floor. Independent integrated
source review approved. Compilation and runtime qualification remain pending.

## Stair and railing alternative profile replacement (v128)

Native reader 128 and JSON/assets extraction 126 retain active-design authoring
twelve: the eight common source-binding fields plus the exclusive
`stair_replacement` child. The closed child one has `version`, `registry_id`,
`alternative_id`, `edits`, `identities`, `child_identities`,
`hosted_instance_identities` and `overlay_identities`. Typed profile edits bind
the actual source; they cannot change object pose or vertical placement.

Replay derives the changed baseline owners and their attached baseline rails.
Their proposed copies use fresh physical and topology identities. Existing
proposed rails retain their identities and move to the proposed host through
known attachment fields. Actual selected hosted rows enter private catalogs;
source catalogs, baseline physical records and other alternatives remain exact.
Known saved view and object annotation rows append corresponding presentations.
Unresolved affected bindings refuse instead of acquiring arbitrary remapping.

Current and retained-history reservation includes physical owners, stair
children, qualified component and overlay identities, assets and computed
canvas aliases. Direct and wrapped source proofs retain the reader floor.
Independent integrated source review approved after retaining ordinary no-copy
edits and source-valid saved view identities. Compilation, interaction, geometry
and storage qualification remain pending.

## Typed ordinary stair and railing profile authoring

Current authoring captures a closed internal intent one with `version`,
`object_id`, `profile_fields` and `quantity_entries`. The complete known family
profile uses explicit null tombstones for optional connection, topology,
placement and host fields. Surviving child metadata is taken from the actual
same typed child identity. Unchanged source numeric encodings remain exact;
known quantity pointers follow child identity across reorder and invalidated
receipts are removed. Opaque bindings cannot acquire changed geometry authority.

The intent itself is not persisted in ordinary entity commands and introduces
no additional reader floor. Existing stair/railing entity dialects remain
authoritative. Actual attachment, levels and native geometry are admitted before
staging; retained-history child reservation remains the enclosing Document's
responsibility. Baseline profile replacement uses the separate source-bound
authoring twelve described above. Captured transform child two is documented
separately; wider mixed-family stair editing remains additional scope.

## Stair and railing alternative demolition (v127)

Native reader 127 and JSON/assets extraction 125 retain active-design authoring
eleven: the eight common source-binding fields plus the exclusive
`stair_demolition` child. The child has exactly `version:1`, `registry_id`,
`alternative_id` and sorted unique `selected_object_ids`. It grants no physical
geometry, replacement, profile or relationship mutation authority.

Replay derives attached baseline flight/landing railings from actual selected
stairs, retaining the physical model, catalogs, views and other alternatives.
Only the saved active alternative's demolition list changes. Selecting a
railing alone leaves its stair intact. Proposed attached rails must be retired
before host demolition; unsupported affected forms or ambiguous ownership
remain explicit refusals. Delete and Cut use the same source-bound intent.
Current/history and wrapped room-review proofs retain this reader floor.
Independent integrated source review approved; no runtime acceptance is claimed.

## Site-aware horizontal hosted components (v126)

Native reader 126 and JSON/assets extraction 124 retain slab geometry intent
three. It has the six earlier intent fields plus the explicit boolean
`coordinate_world_hosted_geometry:true`, and admits only `transform_plan` or
`transform_model`. The actual host's Site frame conjugates the source operation
for type-owned component geometry published in world coordinates. Legacy
host-derived component bodies retain their source-frame geometry contract.
Unchanged catalog rows and definitions remain exact through row-local dialect
seven. Earlier intent one/two keep their original replay meanings.

Derivation archive three retains ordered earlier plan/model records and
requires at least one intent-three record. Later appended operations never
downgrade that archive. Frames and retired entered quantities retain their
existing policies. The reader floor covers current and retained archives,
direct alternative proofs, mixed-family proofs and wrapped room-review proofs,
including geometrically unchanged intent. Copies use the original host's actual
frame before fresh scope enrollment. Independent integrated source review
approved; no runtime or storage qualification is claimed.

## Mixed independent architectural copy composition

Mixed structural, roof and horizontal-assembly copies retain their existing
entity and catalog formats; composition introduces no persisted command
dialect. Each family derives and admits its additive copy against the same
actual source. A complete candidate keeps every physical original, catalog and
registry exact, admits only known presentation-array suffixes, and preserves
all original and independently derived component aliases.

Fresh body owners are globally distinct. Private catalog mappings remain
family-local when multiple host families share one source catalog. Component
IDs remain catalog-local; view overlay IDs remain view-local. Known copied
appearance/annotation templates retain source fields. Bound overlays retain
geometry/offset, while independently admitted unbound overlays carry their
actual transformed coordinates. Existing annotation child vocabulary is
preserved; new child destinations remain reserved. The enclosing producer
reserves retained history/assets and admits one atomic command before scope
enrollment. Mixed wall/room/stair/railing and independent presentation copies
remain additional scope.

Independent integrated source review approved. No compilation, interaction or storage
qualification was performed under the source-only instruction.

## Coordinated structural and architectural alternatives (v125)

Native reader 125 and JSON/assets extraction 123 retain active-design authoring
ten with coordinated inner three. The inner object has exactly `version`,
`wall_authoring`, `roof_replacement`, `slab_replacement`,
`structural_replacement`, `ordinary_roof_edits`, `ordinary_slab_geometry` and
`ordinary_structural_edits`. Absent leaves are null; absent ordinary lists are
empty. At least two families are required. Actual-source ordinary lanes may
compose without replacement in inner three, retaining typed wall room-review
authority. Historical inner one/two still require a replacement. Every
child uses the same captured source; replacement leaves use the same actual
registry and alternative. Historical inner one/two and outer seven/eight keep
their original meanings.

The structural lane carries canonical transform-only edits, either as a
source-derived replacement leaf or an ordinary list. It grants no profile,
demolition or arbitrary entity-change authority. Ordinary columns/beams keep
their identities and attached components; baseline owners receive proposed
physical objects and private component catalogs. Pending wall room decisions
remain pending during combined previews.

Independent family consequences compose only understood source rows. Shared
catalogs may change distinct actual instance placements while retaining their
raw definitions, identities, ordering and envelopes; necessary legacy promotion
uses row-local catalog seven. Conflicting owner/row edits refuse. Original
baseline physical objects and their hosted rows remain exact. The complete
candidate recomputes aliases before publication, and existing current/history/
asset identity checks enumerate every replacement leaf. Native reader floors
also cover retained proofs and their room-review wrappers.

Entirely ordinary mixed groups use the same bounded source-row composition
without new owners; wall groups retain their typed coordinated proof for
mandatory room review. Canvas, numeric transforms and Site
movement share these producers. Mixed independent annotations, references,
assemblies and other object families remain additional source scope.

Independent integrated source review approved. No compilation, runtime, interaction or
storage qualification was performed under the source-only instruction.

## Independent structural copy authoring

Independent column/beam copies use actual-source typed replay and ordinary
atomic entity changes. No new persisted command dialect is introduced. An
identity operation is explicit copy authority, separate from changed-target-only
phase replacement authority. Physical originals and their catalogs remain
exact; private catalogs contain only the copied hosts' selected component rows.
Existing catalog-seven storage continues to require reader 124.

The transient closed copy descriptor has exactly `version:1`, `edits`,
`identities`, `overlay_identities` and `hosted_instance_identities`. Overlay rows
use `{view_entity_id, saved_view_id, overlay_id, proposed_overlay_id}`;
component rows use `{catalog_id, instance_id, proposed_instance_id}`. Actual
source row namespaces stay qualified; computed canvas aliases are never authored
keys. Known presentation rows append mapped copies, and explicit unbound
overlay points transform in their actual view frame. The enclosing producer
reserves retained history/assets and admits the candidate before phase/page
enrollment. Unsupported affected reference families remain explicit gaps.

Independent integrated source review approved. No runtime or storage qualification was
performed under the source-only instruction.

## Source-derived structural hosted components (v124)

Native format 124 and JSON/assets extraction version 122 retain structural
replacement leaf three inside active-design authoring nine. Leaf three has the
six leaf-one keys plus `complete_hosted:true` and `hosted_instance_identities`.
The latter is an ordered array of exact `{catalog_id, instance_id,
proposed_instance_id}` rows. Catalog identities are document owners; embedded
instance identities remain catalog-qualified. Computed canvas identities never
become authored mapping keys. Leaf one and registry-only demolition leaf two
retain their original closed meanings.

Actual baseline host edits derive fresh physical objects and private catalogs
containing only their affected embedded instances. Original definitions,
materials, type profiles, instance overrides, ordering and metadata remain
retained. Proposed catalogs enroll in the saved alternative; only original
physical seeds become demolished. Ordinary hosts sharing the operation retain
their IDs and receive their own independently replayed placement changes.
Saved views, bound overlays, appearances and model annotation placements append
rows for proposed physical owners and the computed component identities.
The complete final map recomputes aliases and preserves every original alias.
Current and retained history, including assets, local instance IDs and escaped
presentation aliases, reserve fresh identities.

Physical authoring uses the actual host's source-frame operation G. With Site
placement F, type-owned world profiles follow `(F*G*F^-1)*A`; legacy geometry
derived from a moving source-frame host follows `G*A*G^-1`, matching its
existing native publication frame. Captured levels supply the native host
datum once. Exact cardinal rotations share the same point and native-solid
evaluation; adjacent arbitrary angles are not snapped and saved angles are not
rewritten.

The new closed `sketch.assemblies.v7` catalog supports retained row-local
envelopes. A type has both `profiles` and `parts` or neither (implicit empty
arrays); an instance has both `root_transform` and `nested_overrides` or neither
(implicit null and empty array). Each placement independently has two or three
translation coordinates and optional `vertical_scale` (implicit one).
Transforming selected placements changes only their understood fields. A
necessary XYZ promotion changes the catalog schema without adding defaults to
unaffected rows or original type definitions. Dialects one through six keep
their original required fields and meanings. Current and historical v7
catalogs require reader 124 even without a retained phase proof.

Independent integrated source review approved, including actual Site frames
and small-delta precision at distant origins. Compilation, interactions, geometry and
storage remain unverified under the implementation-only instruction.

## Source-derived structural alternative edits (v123)

Native format 123 and JSON/assets extraction version 121 retain active-design
authoring nine. It has exactly the eight common source-binding fields and
`structural_replacement`. The structural leaf cannot borrow wall, roof, floor,
relationship or ordinary geometry authority. Older outer dialects are unchanged.

Replacement leaf one has exactly `version`, `registry_id`, `alternative_id`,
`seed_object_ids`, `identities` and `edits`. Each closed structural edit has
`version`, `object_id`, `profile_fields`, `quantity_entries` and `transform`.
Canonical rectangular/circular columns and straight beams admit either a
complete physical profile or the original uniform group operation. Profile
inputs preserve explicit entered receipts and their opaque siblings; computed
transforms create no input measurements. Actual captured levels supply world
placement, and all selected edits derive from the same map.

Only actually changed, active shared baseline owners receive proposed identities.
Ordinary members of the structural cohort retain their IDs. The baseline stays
exact; the actual registry appends proposed/demolished membership. Known saved
view memberships, appearances, bound overlays and model annotation overrides
append mapped proposed rows, retaining original rows, order and envelopes.
Source overlay keys use their actual saved-view vocabulary; fresh destinations
stay strict and reserve current/history/asset/opaque names.

Demolition leaf two has exactly `version`, `registry_id`, `alternative_id`,
`seed_object_ids` and `demolition:true`. It admits only actual active baseline
columns/beams in one saved registry and changes only that alternative's
demolition membership. Every physical and presentation record remains exact.
Supported hosted catalogs retain their records and follow actual host visibility.

Current authoring refuses a same-ID active-baseline column/beam mutation or
removal. Historical ordinary records retain their original meanings. Unsupported
affected opaque references remain separate gaps; hosted component replacement
uses opt-in leaf three above. Independent integrated source review approved the
leaf-one/two source integration. No build, test, runtime or storage round trip
was performed.

## Architectural imperial input spelling (v122)

Native format 122 and JSON/assets extraction version 120 retain quantities
entered with `foot`/`feet`, `inch`/`inches`, or one architectural hyphen between
explicit feet and inches, such as `12'-6"` and `12'-0"`. The leading sign
applies to the entire quantity; the inch component is unsigned. Existing unit
suffixes, exact fractions, scientific decimals and default units retain their
meanings. Missing inch units, repeated separators and signs within the inch
component refuse rather than becoming subtraction or another quantity.

Quantity and hosted-opening input receipts retain the exact original spelling
and rational metres. Their existing shapes and versions are unchanged; the
reader floor identifies the expanded accepted grammar. Receipt-shaped cores
through retained history and historical entity data acquire the floor even if
the edit is undone or abandoned. Typed survey reports and their original reports
also fence distances, numeric curve inputs and closure tolerances whose units
come from the report's input provenance. Unrelated labels, survey source text,
angular curve inputs and arbitrary strings do not acquire a quantity grammar
floor. Scientific receipt detection
retains its existing version 101 requirement when the new spelling is absent.

Source implementation is uncompiled. Manual input, save/reopen, extraction and
recovery remain unverified; no build, test, package or installation was run.

## Source-derived uniform roof scaling (v121)

Native format 121 and JSON/assets extraction version 119 retain roof edit five
and `roof_uniform_transform_derivations`. The edit has exactly the eight
version-four fields plus `uniform_transform`; only the new component is nonnull.
Older edit one through four, rigid transform one and plan resize one retain
their closed meanings.

The uniform operation has exactly `version`, `roof_id` and `transform`. Its six
transform fields retain the actual group pivot, offset, angle, positive non-unit
scale and reflection flags. The actual captured entity map resolves the floor
datum; an entity-only edit cannot supply or infer it. Roof length/run, span,
rise, overhang, thickness and every opening's X/Y/width/depth scale together.
Pitch, placement bindings, materials, child IDs and raw opaque siblings remain
exact. Reflection retains panel-corner and centered gable/hip semantics.

The new owned archive has exactly `version` and `operations`. Each operation
retains the mathematical intent, closed source/result physical frames with
historical vertical datum, and the full raw affected entered receipts. Replay
checks the derivation and receipt values; computed coordinates never become
invented user input. Historical IDs remain historical through clone and design
replacement. Typed residual inspection strips understood historical cores only
on scratch copies and preserves unknown receipt/rational siblings.

Numeric transforms and transformed copies use the same actual world-space
pivot and level datum. Identity copies admit their complete source-derived
graph before applying the operation; original owners and assets remain exact.
All roof members derive from the same actual source before final cohort and
native admission, including combined rigid/scaled operations. Direct, wrapped
and historical proposed commands and current/historical archives raise the
reader floor. Unknown future archives retain their data without granting edit
authority. No build, package, runtime or storage round trip was performed.

## Complete proposed wall presentation references (v120)

Native format 120 and JSON/assets extraction version 118 retain the opt-in wall
replacement leaf seven. Its exact fields are `version`, `complete_presentations`
and `authoring`; the flag must be true. The body retains the closed fields of
wall authoring one through six. Only this wrapper admits nonblank old overlay
map keys of up to 128 bytes, matching saved-view IDs. Replay admits those keys
only from the actual affected overlay inventory. New destinations and every
other identity field remain strict ASCII tokens. Another wrapper, extra fields
or a false flag refuses. Older flat leaves retain their exact admission and
meanings.

The actual source still owns every reference and identity. Known saved views
retain their original object IDs, appearance rows and overlays, adding mapped
copies for proposed owners. Bound overlay targets follow their actual copies;
overlay children reserve distinct entity/view scopes using structural owner
keys. Annotation overrides retain original rows and append mapped model-owner
copies. Output-view overrides remain in their distinct domain. All envelope
fields, retained row order and raw source values remain exact.

Final closure drops only new rows targeting declared entity copies removed by
a semantic relation edit or explicit room relationship omission. Original rows
and all allocated identity reservations survive. Owned overlay, segment and
layer IDs are not mistaken for missing entities. Unknown, future or opaque
affected references still require a typed codec and refuse explicitly.

Current wall geometry, profile, layers/materials and hosted opening edit,
rehost and family-conversion producers opt in. Mandatory source-bound room and
relationship review remains unchanged. Coordinated outer-eight/inner-two edits
retain the same leaf flag through plan discovery, detached preview and final
replay. Direct, wrapped and historical proofs raise the required reader floor;
complete source/history/asset reservations prevent identity reuse after Undo.

Source implementation is uncompiled. Interaction, output, Undo/Redo and storage
round trips remain unverified; no build or package was produced.

## Coordinated wall, roof and horizontal replacement (v119)

Native format 119 and JSON/assets extraction version 117 retain outer phase
constraint authoring version eight in direct, wrapped and historical commands.
Its coordinated inner version two has exactly the five version-one fields plus
`wall_authoring`. Absent families have null replacement fields and empty ordinary
lists. Each present family carries one lane, at least two families participate,
and at least one requires actual baseline replacement. Older outer and inner
dialects retain their closed fields and meanings.

The wall child is canonical outer authoring one or two, containing only an actual
wall geometry move and any required typed replacement/room decisions. Every
child source binding equals the enclosing captured source. Actual replacement
leaves must name the same registry and saved alternative. Each family derives
its complete candidate independently from the unchanged actual source.

Disjoint physical consequences compose with explicit codec-known registry,
annotation and saved-view rows. Retained row order and envelope fields stay
exact; a source row may have one consequence, and fresh destinations are disjoint.
Unsupported physical/catalog overlap refuses. Baseline owners, source fences,
whole-map ownership and full retained identity lifetime remain authoritative.

Canvas preview carries a detached physical map and the full enclosing intent;
it does not construct a publishable snapshot from unreviewed room facts. Wall,
roof, floor and hosted component presentations use actual staged geometry and
qualified render aliases. Release completes the existing wall room/relationship
review, inserts that child into the same captured coordinated proof, and admits
one complete undoable command. An ordinary wall with a replaced roof or floor
retains the complete mixed geometry proof through a separate typed room suffix.
The original mixed selection supplies review authority even when its primary
owner is a roof or floor. Numeric previews retain the detached intent until
review completes. Admission validates the complete geometry stage independently
of room decisions and prevents the suffix from changing physical owners.
Source implementation is uncompiled and
interaction, output and storage remain unverified.

## Singleton roof material relationships (v118)

Native format 118 and JSON/assets extraction version 116 retain roof join
properties version three and roof demolition replacement authoring version eight,
including when present only in retained history. Join three has exactly
`version`, `style`, `roof_ids` and `material_assignment`: fused style, one actual
roof, and the established closed version-one material assignment. It represents
the effective material of that roof without rewriting its physical owner.
Earlier join dialects retain their two-to-sixteen member rule and closed fields.

Standalone demolition version three adds `preserve_singleton_material: true` to
the phase-qualified version-two envelope. Replacement demolition version eight
adds the same field to version seven. Both require actual phase-qualified join
semantics. When deletion leaves one material-bearing survivor, the producer
retains or creates a material relationship using actual source material and
survivor identity. Proposed relationships reference independent baseline copies
or retained ordinary/proposed roofs as appropriate; original baseline owners
and joins remain exact. Fresh relationship identities retain the same full
history and asset reservations. Older intent dialects retain their meanings.

Current Cut/Delete uses this source-derived path. The changes have not been
compiled or qualified through interaction, storage or output.

## Coordinated roof and horizontal design replacement (v117)

Native format 117 and JSON/assets extraction version 115 retain outer phase
constraint authoring version seven in direct, wrapped and historical commands.
The outer source-bound envelope contains only `coordinated_replacements` in
addition to its eight shared fields. Its ordinary semantic intent carries only
the message. The inner version-one object has exactly `version`,
`roof_replacement`, `slab_replacement`, `ordinary_roof_edits` and
`ordinary_slab_geometry`.

Each family has exactly one lane: a canonical non-demolition replacement leaf,
or a nonempty canonical ordinary typed edit list. At least one family requires
replacement, and two replacement leaves must name the same actual registry and
saved alternative. Historical leaves keep their original codecs and meanings.
Both complete family candidates derive from the unchanged actual source.

Physical consequences must be disjoint. Shared registry and presentation
containers merge only through admitted append paths: actual active registry
rosters, annotation overrides, and per-view object, appearance and overlay rows.
All source array prefixes, raw order and remaining envelope values stay exact.
Conflicting physical or catalog consequences and overlapping fresh identities
refuse. Whole-map ownership, baseline preservation, identity lifetime and native
admission precede one undoable publication. Canvas dragging, transform actions
and Site movement use the same producer; previews and selection redirection
include both families. Other shared-baseline family coordination remains separate.
These changes have not been compiled or qualified through runtime or storage.

## Phase-qualified roof demolition (v116)

Native format 116 and JSON/assets extraction version 114 retain roof replacement
authoring version seven through direct, wrapped and historical commands.
Its eight exact fields are the seven version-four demolition fields plus
`phase_qualified_joins: true`. Standalone roof demolition intent version two
similarly adds that discriminator to version one's six fields. Older dialects
retain their closed fields and original interpretation.

Replay derives the active join cohort and each member's actual saved role.
Original baseline roofs and joins remain exact and become demolished only in
the saved active alternative. Baseline survivors get independent proposed
copies; ordinary/proposed survivors keep their source IDs, bodies, openings
and roles. New survivor joins alone receive actual phase ownership metadata.
Mapped identities for omitted seed copies stay reserved across Undo. Qualified
presentation extends only actual new owners and overlays, while complete
source/candidate ownership and native admission use the same retained map.

A retained singleton cannot inherit a different join material by mutating its
preserved owner; that case requires a separate phase-specific representation.
Source implementation remains uncompiled and runtime/storage unverified.

## Independently scaled assembly heights (v115)

Native format 115 and JSON/assets extraction version 113 retain assembly catalog
schema `sketch.assemblies.v6` and independent instance schema
`sketch.assembly-instance.v2`, including when present only in retained history.
Catalog six keeps the XYZ placement and nested envelope from catalog five;
every hosted placement additionally carries a positive finite `vertical_scale`.
Version-six transforms may carry this factor on roots, parts and nested overrides.
Independent instance two allows the same optional transform factor. Earlier
enclosing schemas keep their closed fields and reject the new field, even one.

The existing `scale` applies to XY. Actual Z scale is `scale * vertical_scale`;
an absent factor in earlier schemas is one. Y reflection precedes yaw, then
translation. Composition multiplies corresponding scales; hosted legacy-solid
placements use actual `G*A*G^-1`, while type-owned profile instances use `G*A`.
Actual native geometry and calculated volume use the same affine transform.
Entered quantities remain authored quantities and are not rescaled.

Horizontal plan scaling uses XY scale `s` and factor `1/s`, retaining physical
Z and thickness. Spatial model scaling retains uniform XYZ semantics. Ordinary
and proposed hosted producers patch actual placements; original baseline rows,
definitions, materials, overrides and unchanged raw numeric values remain exact.
Needed upgrades add only required codec envelope fields. Existing catalog six
and independent instance two remain in that dialect when factors return to one.
Native previews, plan projections, schedules, IFC extrusion heights and component
properties consume the same scale. These source changes have not been compiled
or qualified through interaction, export or storage round trips.

## Phase-qualified roof joins (v114)

Native format 114 and JSON/assets extraction version 112 retain roof replacement
authoring version six and understood phase-qualified join ownership through
direct, wrapped and historical commands, including Undo. Version six has the
nine version-five fields plus `phase_qualified_joins: true`. The baseline combined
edit list is nonempty; the ordinary list may be empty. Historical profile and
opening arrays remain empty. Earlier authoring retains its saved meaning.

An affected active join copies its actual target-registry baseline members and
retains actual ordinary/proposed member identities. Original roofs and joins
remain exact. Only fresh joins receive the extension
`roof_join_phase_ownership: {version: 1, registry_id: "..."}`. Replay derives the
role partition, complete mapping and retained member roster from the source,
then admits actual candidate geometry, contexts, materials and registry changes.

Known ownership metadata contains exactly `version` and `registry_id`; identifiers
use the bounded document alphabet. Sharing requires a known qualifier on at
least one join and unique membership of both joins in the same actual registry.
Two baseline joins always conflict. Two proposed joins exclude each other only
in different alternatives. A baseline and proposed join exclude each other only
when that proposal's alternative demolishes the baseline join. This admission
covers baseline and every retained alternative, regardless of current selection.
Ordinary, unqualified, foreign-registry or coactive sharing still refuses.
Future qualifier versions remain opaque and confer no sharing authority.

Current-design edit producers omit inactive preserved joins only in this
validated qualified cohort. Independent fresh join copies drop only understood
version-one ownership metadata. Physical deletion cannot remove a roof still
referenced by a preserved join; phase-preserving removal remains a separate
typed lifecycle. Source implementation has not been compiled or qualified
through interaction or storage round trips.

## Mixed baseline and ordinary roof authoring (v113)

Native format 113 and JSON/assets extraction version 111 retain roof replacement
authoring version five through direct, wrapped and historical commands,
including Undo. It has version three's eight fields plus `ordinary_roof_edits`:
`version`, `registry_id`, `alternative_id`, `seed_roof_ids`, `identities`,
`roof_profiles`, `roof_opening_edits`, `roof_edits` and `ordinary_roof_edits`.
Both combined edit lists are nonempty and the historical profile/opening arrays
are empty. Earlier replacement dialects keep their exact saved meaning.

Actual full-map typed replay precedes role classification. Changed shared
baseline targets receive proposed copies in the saved active alternative;
ordinary/proposed targets retain their original IDs and receive actual derived
edits in the same publication. Unchanged targets confer no replacement authority.
Target lists are disjoint and independently checked against all saved registries.
New opening identities from both lists remain reserved through history, assets
and retained semantic intents. Source/final roofs and affected joins receive
native, context and material admission.

Complete baseline join cohorts and independent ordinary joins are supported.
A source join spanning baseline and ordinary members still requires a separate
qualified join replacement interface. Compilation, interaction and storage
round trips remain unverified.

## Coordinated ordinary and proposed hosted movement (v112)

Native format 112 and JSON/assets extraction version 110 retain horizontal
replacement authoring version six. It adds
`coordinate_ordinary_hosted_geometry:true` to the version-five ten-field
envelope. Both geometry lists must be nonempty; the qualified hosted copy map
may be empty. Versions one through five retain their original replay meaning.
Retained direct, wrapped and undone version-six authoring raises the reader
floor independently of the current live entities.

Ordinary movement independently derives physical geometry and actual attached
catalog placements from the full source. Rigid plan and physical XYZ/uniform
model transforms carry the hosted instances; vertex and axis edits keep their
placement while legacy host geometry follows the resulting actual slab.
Strict source/candidate expansion and native admission use actual contexts,
hosts, materials and overrides. Affected future or malformed bindings refuse;
unaffected catalogs remain opaque.

Mixed replay combines the proposed baseline copies with those independently
derived ordinary catalog changes in one publication. A catalog shared by
baseline and ordinary hosts retains the baseline-hosted values while the
ordinary placements change. A needed XYZ upgrade adds the required version-five
envelope and zero Z to other placements without changing their values. Only
baseline seed owners acquire demolition membership. Definitions, material
values, overrides, unrelated payload and instance identities remain unchanged.
Hosted nonunit plan scaling remains a separate representation gap. This source
has not been compiled or qualified through interaction/storage round trips.

## Proposed horizontal assemblies with hosted components (v111)

Native format 111 and JSON/assets extraction version 109 retain horizontal
replacement authoring version five, including retained, wrapped and undone
phase commands. Its exact ten fields are `version`, `registry_id`,
`alternative_id`, `seed_slab_ids`, `identities`, `slab_profiles`, `slab_stacks`,
`slab_geometry`, `ordinary_geometry` and `hosted_instance_identities`.
Exactly one primary edit family is nonempty; ordinary edits accompany only
geometry. Qualified hosted rows are sorted objects with `catalog_id`,
`instance_id` and `proposed_instance_id`. Versions one through four retain
their existing wire rules.

Actual source discovery derives each affected catalog and its selected hosted
instances. The allocator reserves entity, child, qualified component and
presentation identities throughout history and assets. Replay creates a
private catalog containing only the seed-hosted instances, with fresh instance
and host identities. Original catalogs and their unrelated instances remain
exact. Only seed horizontal owners acquire demolition membership; copied
owners and catalogs acquire proposed membership in the selected alternative.

XYZ and rigid plan operations compose the copied placements with the actual
host transformation. Profile, stack and outline edits retain placement while
legacy host-based geometry follows its changed host. The final native candidate
is admitted against actual hosts, definitions, materials, overrides and context.
Nonunit plan scaling of hosted components currently refuses because a 3D
similarity would incorrectly scale profile height. Compilation, interaction
and storage round trips remain unqualified.

## Spatial hosted assembly placements (v110)

Native format 110 and JSON/assets extraction version 108 retain
`sketch.assemblies.v5`. It uses the version-four nested type and instance
envelopes, with exactly three XYZ coordinates in each host placement's
`translation_m`. Version-one through version-four codecs retain their exact
two-coordinate placement rules and implicit zero Z. Canonical encoding selects
version five when a hosted placement has nonzero Z; actual raw version-five
payloads remain version five during typed editing even if Z becomes zero.

The native host preview, profile expansion and schedules use the same explicit
Z placement. Mathematical placement editing preserves the host identity and
declared quantities. For legacy repeated-host solids it derives `G A G^-1`,
so applying the new placement to the transformed host yields the transformed
original assembly. Authored profile instances instead receive `G A`; bounded
actual expansion independently determines the applicable branch. Exact no-ops
preserve raw payloads. A needed version-three upgrade adds only empty nested
type/instance envelopes, while existing definitions, materials, overrides and
untouched numeric representations remain exact.

Independent horizontal cloning discovers actual hosted catalogs and qualified
`(catalog_id, instance_id)` identities. Fresh owner/catalog/layer/overlay and
qualified instance mappings are complete and independently rediscovered. Each
copied catalog contains only instances hosted on the selected source slabs;
the original catalog and unrelated instances remain exact. Catalog-local
definition/material identities remain local and retain their raw values.
Unknown affected references refuse rather than acquiring rewrite authority.
The desktop allocator reserves all destinations throughout revisions and
assets, including qualified instance names. XYZ/yaw/reflection/uniform model
copy transforms use the typed placement producer and native candidate admission
before one source-bound creation command. Version-five catalogs in retained
history raise the reader floor even after Undo.

This is source implementation. Compilation, interaction and storage round trips
remain unqualified. Shared-baseline hosted replacement uses the separate typed
authoring extension described above.

## Horizontal assembly model transforms (v109)

Native format 109 and JSON/assets extraction version 107 retain analytical slab
model transformations. Geometry intent version two has the same six keys as
version one: `version`, `slab_id`, `kind`, `vertex`, `transform` and `resize`.
Its exclusive `transform_model` kind carries `pivot_m` and `offset_m` as XYZ
coordinates, `rotation_radians`, positive `uniform_scale`, `flip_horizontal`
and `flip_vertical` in `transform`; the other operation fields are null.
Version-one plan operations retain their existing wire and profile semantics.

Model operations move and uniformly scale the actual footprint, holes, total
thickness, ordered layer thicknesses and elevation. Resolved floor/level
placement supplies the world elevation used for scaling. The saved placement
and source context remain exact; the resulting native elevation offsets that
actual shift. Pure translations avoid subtraction of the placement shift.
Detached replay admits only absent or explicit absolute placement; level mode
requires the complete actual entity map.

`extensions.slab_geometry_derivations` version two retains unchanged version-one
records and new model records, with at least one model operation required.
Model source/result frames contain `boundary`, `holes`, `profile` and
`elevation_shift_m`. The exact profile fields are `thickness_m`, `thickness`,
`elevation_m`, `elevation` and `layers`; absent source aliases or inventories
are null, and an existing layer array records ordered thicknesses without live
layer identities. Actual numeric representations survive unchanged fields.
Every result is independently derived from its recorded operation, with finite
coordinates, positive thickness, consistent aliases/layer totals and native
analytical solid admission. Changed known entered-measurement receipts retire
verbatim into the record; unsupported affected bindings refuse the edit.

The desktop uses this source replay for 3D movement, uniform scaling and
transformed independent copies, including floor-relative sources. Plan canvas
scaling continues to preserve physical thickness. Direct/wrapped shared-baseline
geometry intents and retained derivation archives raise the reader floor,
including historical revisions after Undo. This section documents source
implementation; compilation and storage round trips are not yet qualified.

## Retained horizontal-layer thickness input (v108)

Native format 108 and JSON/assets extraction version 106 retain slab profile
version two inside existing slab replacement one/phase envelope five. The exact
fields remain `version`, `slab_id`, `thickness`, `elevation` and
`layer_thicknesses`. Each ordered row has `layer_id` and `thickness`; a version-two
null thickness retains the actual source row's native value and quantity receipt.
The complete source inventory and order are required. Changed rows carry actual
entered quantities. Version two requires a retained row and an authored scalar
or layer dimension; retention alone is not an edit. Version-one wire and replay
remain unchanged and do not admit null row thickness.

Source/native/receipt admission precedes staging. Inferred capture retains
unchanged rows and requires genuine candidate receipts for changed rows. Exact
source no-ops preserve payloads. Shared-baseline replay maps actual layer IDs
once while retaining source values. Direct/wrapped historical profile intents
raise the required reader floor, including after Undo.

The desktop horizontal profile parses only changed fields. Native values are
displayed without inventing quantities for untouched rows; editing one layer
therefore no longer reconstructs the remaining native dimensions. Layer names
are readable and the read-only sum follows imperial or metric display units.

## Source-derived wall layer inventories (v107)

Native format 107 and JSON/assets extraction version 105 retain wall layer
inventory/material decisions and retired entered layer measurements. Stack
intent version one has exactly `version`, `wall_id`, `thickness` and `layers`.
Each ordered row has `layer_id`, `thickness`, `material_mode` and `material`.
Null thickness retains the actual existing value and receipt. Material mode is
`retain`, `clear` or `set`; only `set` carries an actual catalog/material reference.
Rows declare the complete resulting inventory and order. Omitted old rows are
removed; an empty array clears the stack. New rows require an entered positive
thickness and explicit material choice. Total thickness is retained or explicitly
entered and is never inferred from the layer sum.

Qualified indexed thickness receipts follow their actual layer identity through
reordering. Removed receipts are retained verbatim in
`extensions.wall_layer_stack_retirement`, with version-one fields `version` and
`receipts`. Each retired row has `layer_id`, `pointer` and `receipt`. Its layer
identity is historical provenance, never a live child binding or copy-remapping
target. Opaque receipt siblings remain subject to affected-reference checks.
Unknown affected bindings and archive versions refuse without mutation.

Shared-baseline changes use exclusive wall replacement version six inside phase
envelope two. The exact fields are `version`, `registry_id`, `alternative_id`,
`seed_wall_ids`, `identities`, `room_review_intent`, `room_constraint_decisions`
and `wall_stacks`. Replay independently derives the changed baseline roots from
actual saved membership. Existing layer IDs map once; actual new row IDs retain
their declared fresh identities. Removed original child mappings remain reserved.
Original baseline owners and other alternatives stay exact. Native source/result
profiles, hosted cuts, opening assemblies, affected joins, context and catalog
materials undergo admission. Room and relationship review finishes the same
atomic edit before final publication. Prior replacement wires remain unchanged.

The desktop table exposes Add, Remove, Up and Down, editable total and layer
thicknesses, actual material choices and a read-only layer sum. Source/saved/
workspace fences and retained-history reservations protect publication.
Native retirement archives and direct/wrapped retained replacement intents raise
the reader floor throughout history, including after Undo.

## Mixed-role horizontal geometry editing (v106)

Native format 106 and JSON/assets extraction version 104 retain exclusive slab
replacement version four inside phase envelope five. Its exact fields are
`version`, `registry_id`, `alternative_id`, `seed_slab_ids`, `identities`,
`slab_geometry` and `ordinary_geometry`. Both geometry lists are nonempty,
disjoint and contain actual changed targets. Earlier replacement versions and
their replay policies remain unchanged.

Replay independently derives each target's role from the actual saved source.
The baseline geometry list requires one registry and its saved active
alternative, and its exact changed owner IDs establish the replacement seeds.
Ordinary or proposed owners retain their actual identities. A baseline owner
in a registry without an active alternative may use the ordinary lane. Inactive,
ambiguous or multiply registered owners refuse; supplied role lists provide
no authority. Exact no-ops are discarded before classification.

The same atomic operation derives fresh proposed baseline owners and applies
ordinary geometry to its actual owners. Original shared baselines, other
alternatives and unaffected content remain exact. Geometry archives and qualified
presentation copies retain existing provenance rules. Direct and wrapped
retained history raise the reader floor even after Undo.

## Retained wall-layer thickness input (v105)

Native format 105 and JSON/assets extraction version 103 retain wall profile
version three inside existing wall replacement two/phase envelope two. Its
exact fields are `version`, `wall_id`, `thickness`, `height`,
`layer_thicknesses` and nullable `top_rise`. Existing profile versions one and
two retain their original wire and admission policy.

Each ordered layer row still has `layer_id` and `thickness`. A version-three
null thickness retains the actual source row's native value and quantity
receipt. The complete source inventory and order are required. Changed rows
carry actual entered quantities; untouched rows need no reconstruction from
a floating-point value or reentry merely because another row changes. Version
three requires at least one retained row. A retention-only empty edit refuses;
an authored equal dimension remains an exact no-op.

Actual source profile/receipt admission precedes staging. Strict native
source/result dependency checks cover hosted openings and affected joins,
including all copied proposed walls. Inferred capture records retained rows
only where actual source thickness is unchanged; changed rows require actual
candidate receipts. Total thickness remains explicit or retained, with no
implicit redistribution. Historical direct/wrapped proposed intents raise the
required reader floor, including after Undo.

The desktop wall profile uses thickness, height, top-rise and existing-layer
measurement fields instead of JSON. Base elevation is displayed from actual
resolved placement. Source, saved-state and workspace fences precede final
controller publication. Temporary profile admission leaves final room and
relationship completion to the enclosing atomic edit.

## Source-derived horizontal outline edits (v104)

Native format 104 and JSON/assets extraction version 102 retain horizontal
assembly vertex edits, rigid plan transforms with positive uniform plan scale,
and positive axis resizing in a captured rotated frame. These operations retain
the actual slab/floor/ceiling/foundation profile, thickness, elevation, layer
inventory, materials and drawing context. Circular arcs retain analytical
sweeps under rigid and uniform transformations. Unequal axis scaling of an arc
refuses rather than silently changing its curve representation.

The strict version-one geometry intent has exactly `version`, `slab_id`,
`kind`, `vertex`, `transform` and `resize`. Exactly one operation is nonnull.
`move_vertex` contains `vertex_index`, `proposed_position_m` and nullable
`hole_index`. `transform_plan` contains `pivot_m`, `rotation_radians`,
`flip_horizontal`, `flip_vertical`, `offset_m` and `uniform_scale`; scale
occurs about that pivot before rotation/reflection and translation.
`resize_plan` contains `scale_x`, `scale_y`, `anchor_m` and
`frame_rotation_radians`. This records mathematical intent, not a fabricated
entered measurement.

Changed understood coordinate and sweep receipts validate against their actual
source scalar and retire verbatim into `extensions.slab_geometry_derivations`.
Its version-one envelope contains exactly `version` and `operations`; each
operation contains `operation`, `source`, `result` and `receipts`. Source and
result frames contain closed `boundary` and `holes` geometry. Each segment
contains `start`, `end` and `sweep_radians`; live opaque segment siblings stay in
the owner. Retired receipts use the exact `quantity_entries` object. Known
bindings are `/boundary/I/start/0|1`, `/boundary/I/end/0|1`,
`/boundary/I/sweep_radians` and their `/holes/H/I/...` equivalents. Unsupported
affected bindings refuse; unchanged numbers and receipts remain exact.

Each retained result is independently reconstructed from its recorded source
and mathematical operation. The archived operation's `slab_id` is historical
provenance, never a live owner binding or copy-remapping target. Opaque receipt
siblings remain subject to affected-reference checks. The bounded archive
cannot replace the actual live source used for a new edit.

Shared-baseline geometry edits retain phase envelope five with exclusive
version-three slab replacement. Its six fields are `version`, `registry_id`,
`alternative_id`, `seed_slab_ids`, `identities` and `slab_geometry`. Source
inspection establishes the changed targets, actual owner/layer/overlay roster,
fresh mapping and saved active alternative. Baseline owners and other
alternatives remain exact; supported presentation copies are additive.
Direct/wrapped replacement proofs and retained geometry archives raise the
reader floor throughout history, including Undo. Earlier profile/stack wires
and their reader floors remain unchanged.

## Source-derived horizontal layer stacks (v103)

Native format 103 and JSON/assets extraction version 101 retain source-derived
ordered layer inventory/material edits and retired entered layer measurements.
The strict version-one stack intent has exactly `version`, `slab_id`,
`thickness` and `layers`. Each ordered row has exactly `layer_id`, `thickness`,
`material_mode` and `material`. Null thickness retains the existing exact native
dimension and receipt. Material mode is `retain`, `clear` or `set`; only `set`
has a version-one actual catalog/material reference.

Rows declare the complete resulting inventory and order. Missing original rows
are explicitly removed; an empty array clears the stack. New rows require an
actual entered positive thickness and explicit material choice. Total thickness
is retained or explicitly entered, never inferred from the layer sum. Native
layers must sum to that total. Source/result geometry, aliases, resolved levels,
actual drawing context and actual catalog materials undergo admission.

Known indexed quantity bindings follow actual layer identity through reordering.
Changed measurements retain opaque receipt siblings. Removed known receipts
append verbatim to `extensions.slab_layer_stack_retirement`, whose exact
version-one envelope has `version` and `receipts`; each row has `layer_id`,
`pointer` and `receipt`. This archive is bounded to 4,096 rows and 1 MiB. Its
layer IDs are historical provenance. Future or unknown affected bindings refuse
without losing data; unedited quantities are never reconstructed from a float.

Shared baseline stack edits retain phase envelope five with an exclusive
version-two slab replacement. Its six fields are `version`, `registry_id`,
`alternative_id`, `seed_slab_ids`, `identities` and `slab_stacks`. The original
source child mapping remains complete, including reserved names for removed
layers. Surviving original layer identities map to new proposed children; new
authored rows keep their declared fresh IDs. Both are reserved across history,
including Undo. Original owners and other alternatives stay exact. Supported
historical retirement IDs remain provenance in proposed copies.

The desktop layer table supports add, remove, reorder, entered thickness and
catalog material selection, with an explicit total and read-only layer sum.
Captured document, saved-state and workspace fences precede publication.
The public layer API captures actual numeric input tokens and uses the same
semantic editing lane. Direct/wrapped replacement proofs and retained retirement
archives raise the required-reader floor. Earlier profile wire and floors stay
unchanged. This documents source contracts; compilation and runtime behavior
remain unverified for this source batch.

## Retained baseline horizontal assembly demolition (v102)

Native format 102 and JSON/assets extraction version 100 retain demolition of
shared baseline slabs, floors, ceilings and foundations inside the actual saved
alternative. Phase envelope six contains only `slab_demolition` beside its
existing captured-source bindings and empty ordinary semantic inventory. Its
strict version-one decision has exactly `version`, `registry_id`,
`alternative_id` and sorted unique `slab_ids`.

Replay independently derives actual active baseline membership. Only the saved
active alternative's `demolished_ids` list receives an exact additive change;
original owners, assemblies, other alternatives, presentation and opaque data
remain retained. Supported slab-hosted assemblies remain intact and use existing
inactive-host projection in native geometry, schedules and saved views. Native
profiles, resolved placement, context and actual materials undergo admission.
No fresh identities or detached entity payload grant demolition authority.

Delete and Cut use the same captured source and operation. Cut publishes the
clipboard only after admitted document publication. Ordinary/proposed-only
deletion keeps its existing lane. Baseline removal through the general edit API
uses the same typed demolition proof. The operation's site footprint includes
the retained owners whose active role changes. The required-reader floor covers
direct and wrapped proofs across retained history, including Undo.

This is a source contract. Compilation, interactive behavior, clipboard/history
behavior and storage round trips have not run for this source batch.

## Source-derived proposed horizontal assemblies (v101)

Native format 101 and JSON/assets extraction version 99 retain source-derived
slab, floor, ceiling and foundation profile replacements. Version five of the
source-bound phase envelope adds only `slab_replacement`, exclusively with the
ordinary semantic edit inventory empty. Earlier envelope dialects and their
required readers remain unchanged.

Its strict replacement record has exactly `version`, `registry_id`,
`alternative_id`, `seed_slab_ids`, `identities` and `slab_profiles`. Seeds must
be changed, active baseline owners in the actual saved alternative. Replay
derives the new native owners from the original map; callers cannot supply
geometry payloads. Distinct slab, ordered assembly-layer and bound-overlay
identities are explicitly mapped and reserved across retained history, including
Undo. Originals and other alternatives stay exact. Qualified saved-view object
lists, appearance rows, bound overlays and annotation overrides gain additive
copies; unsupported affected references produce blocking diagnostics.

The strict version-one profile record has exactly `version`, `slab_id`,
`thickness`, `elevation` and `layer_thicknesses`, with explicit null inputs.
Thickness and every layer thickness are positive actual entered quantities;
base elevation permits signed and zero quantities. Complete ordered existing
layer inputs retain their identities, materials and order. Layer sums must fit
the explicit or retained slab thickness; no distribution or total is inferred.
Scalar aliases must agree. Unedited numeric representations, footprint/holes,
kind, context and opaque metadata remain exact. Understood changed quantity
receipts retain opaque siblings; unsupported affected receipt versions refuse.
Source, final geometry, levels and actual material catalogs undergo admission.

Ordinary and already-proposed profile edits retain their owner identity. A
source-equivalent edit returns no change before allocation. The desktop supplies
the actual parsed thickness/elevation inputs, and redirects selection only after
successful publication. A compact properties dialog exposes ordinary dimension
fields and an ordered layer table; the separate stack editor remains reachable
for inventory and material changes. Untouched native values remain untouched,
including values outside the editable exact-rational range.

Reader 101 also supports exact scientific-decimal quantities. The parser
normalizes the lexical coefficient, decimal point and exponent before bounded
rational construction, cancelling denominator factors without converting the
entered number through binary floating point. It retains the original expression.
Scientific receipts in entities and wrapped command proofs raise the reader
floor throughout retained history, including Undo. This scan uses the existing
JSON value budget without imposing a new nesting limit on ordinary opaque
metadata. Earlier receipts retain their original reader floors.

This documents source contracts; compilation, interaction, history and file
round trips have not run for this source batch.

## Source-derived roof footprint resizing (v100)

Native format 100 and JSON/assets extraction version 98 retain mathematical
roof plan resizing. Its strict version-one operation has exactly `version`,
`roof_id`, `scale_x`, `scale_y`, `anchor_m` and `frame_rotation_radians`.
Factors are positive finite scalars; the anchor is actual world XY metres.
It contains neither a detached candidate nor an invented entered dimension.

Side and corner grips derive the requested native footprint from actual source
bounds, including overhang and normal-thickness projection. The opposite
footprint edge/corner stays anchored. Roof form, Z, rise, normal thickness,
overhang, orientation, material, context, schema and opening identity/order
remain exact. Opening rectangles follow their actual resized core dimensions.
Pitch changes only when its controlling run/span changes. Unedited numeric
representations and opaque child/owner data remain intact.

A sloped panel's projected run is `run + thickness * rise / hypot(run, rise)`.
Low-rise thick panels can have more than one admissible run for the same
footprint. The bounded solve stays on the monotone branch containing the actual
source run; it does not jump to another pitch. A source at an unresolved fold
or a target outside that branch refuses with an explanation. Generated final
footprint dimensions must match the requested scale within native tolerance
and coordinate roundoff. Crossing an opposite grip edge also refuses.

Changed understood `quantity_entries` and `roof_opening_input` receipts move
verbatim into `roof_plan_resize_derivations`, preserving opaque siblings.
The strict archive envelope has exactly `version` and `operations`; each
operation has exactly `operation`, `source`, `result` and `receipts`. Historical
physical frames contain closed canonical roof fields and opening rectangles.
Every result is independently derived from its actual historical source and
operation through the same native geometry stage. Archived inputs bind their
original scalars. Unsupported affected receipts refuse; historical identities
remain provenance through independent and proposed copies. Known rigid
movement history remains exact. Positive future archive versions are retained
read-only. The floor follows every retained revision and nested/composed proof,
including after Undo.

Baseline resizing uses combined roof edit version four inside replacement
record three: exactly `version`, `roof_id`, `profile`, `openings`, `pose`,
`form`, `transform` and `resize`. Only `resize` is nonnull. Historical combined
dialects one, two and three retain their exact wires and replay behavior.
Resize cohorts enforce unique active owners, actual level-resolved source/final
roofs and joins, and aggregate target/archive budgets. Other simultaneous
owner edits are admitted as one complete final cohort. Ordinary resizing uses
the same typed mathematical replay rather than deleting historical inputs.

Unchanged historical archives can reuse successful validation by exact dialect
and exact serialized bytes in a bounded synchronized in-process memo. Failed,
changed, empty or oversized archives never acquire cached approval. Current
source/result geometry and all live ownership/context checks still run. This
is a source optimization; runtime responsiveness has not been measured.
Native geometry staging and persisted replay share one authoritative target.

Canvas preview projects actual proposed owners and preserves the captured saved
view and post-commit selection mapping. The installed candidate remains
unchanged. Compilation, grip interaction, Undo/Redo and storage round trips
remain unverified. Uniform three-dimensional scale, mixed baseline/non-roof
authoring and unsupported live references remain separate implementation work.

## Source-derived roof movement and independent copies (v99)

Native format 99 and JSON/assets extraction version 97 retain mathematical
roof movement, rotation and reflection. The operation contains exactly
`version`, `roof_id` and `transform`; the transform contains exactly `pivot_m`,
`offset_m`, `rotation_z_radians`, `scale`, `flip_horizontal` and `flip_vertical`.
Scale is one. This is an explicit mathematical operation on the actual source,
not a fabricated measurement receipt for a computed coordinate.

The source-derived result changes only roof placement, orientation and the
reflected opening Y coordinates. Original profile dimensions, opening IDs,
schema, context, material and opaque metadata remain retained. Affected
understood coordinate receipts are moved verbatim into the owned extension
`roof_rigid_transform_derivations`. Its strict version-one envelope contains
exactly `version` and `operations`; each operation record contains exactly
`operation`, `source`, `result` and `receipts`. The receipt record contains
exactly `quantity_entries` and `roof_opening_input`. Every closed historical
source/result frame is independently derived from its mathematical operation,
and every archived input is checked against its actual original scalar.
Unsupported affected bindings refuse. Opaque receipt siblings remain retained.
Historical owner and child IDs are provenance and are never remapped by a copy.
Future positive archive versions remain intact in a read-only document.

Baseline movement uses roof replacement record three with combined roof edit
version three: exactly `version`, `roof_id`, `profile`, `openings`, `pose`,
`form` and `transform`. Only `transform` is nonnull. Earlier combined dialects
and their floors remain unchanged. The floor follows retained archives and
direct, nested or composed replacement proofs, including after Undo. Native
source and final resolved roofs and affected joins are admitted as a complete
map. Original baseline owners and other alternatives remain retained.

Independent roof copies derive their exact entity and child mappings from
explicit active owners. Whole selected joins are copied; partial selections
retain the effective source join material as independent roofs. Owned opening
and bound overlay IDs are fresh. Qualified view and annotation presentation is
appended while original rows remain exact. Every new owner receives actual
current phase and page enrollment. Controller validation accepts only this
captured copy and its exact source-derived enrollment. Copy destinations cannot
reuse a reserved name in retained history or opaque source data. Existing local
assets stay shared. Archived movement inputs remain byte-equivalent.

Saved-view previews retain both their sheet-view owner and local view ID.
Admitted prepared publication redirects selection to the actual proposed roof
after commit. No screen alias replaces a physical document identity.
This source contract has not been compiled or exercised. Non-unit scaling,
mixed baseline/non-roof edits and unsupported live reference families remain
separate implementation work.

## Typed proposed roof form conversion (v98)

Native format 98 and JSON/assets extraction version 96 retain a conversion
between sloped panel, gable and hip roofs in an active design alternative.
The source-bound phase intent four and roof replacement record three retain
their outer fields. A combined roof edit with conversion is strict version two:
exactly `version`, `roof_id`, `profile`, `openings`, `pose` and `form`. `form`
is nonnull and `profile` is null. Historical combined edits retain their exact
five-field version-one wire and earlier floor. The floor follows retained
direct, nested and composed proofs, including after Undo.

The form component is strict version one with exactly `version`, `roof_id`,
`target_form`, `length`, `span`, `rise`, `overhang` and `thickness`. All five
target dimensions have exact quantity receipts. Length means panel run or
gable/hip plan length. A hip ridge is shorter than this footprint dimension.
Length, span and thickness are positive; overhang can
be zero and a sloped panel can have zero rise. Gable/hip rise is positive.
Dependent pitch derives from the target dimensions. A genuinely different
known form is required; same-form size editing retains its profile contract.

Conversion retires the source form's active run/length field and migrates its
known receipt to the target pointer, retaining opaque receipt siblings.
Unsupported affected receipts or inactive destination collisions refuse.
Source pose, schema version, actual opening roster, context, materials and
unrelated metadata remain retained. Form, optional opening edits and optional
pose changes all stage against the same actual original and merge only owned
deltas before final native admission. No intermediate roof becomes a source.

The dialog supplies actual parsed target quantities or genuine preserved exact
source inputs. Where no exact source input is available, the current physical
dimension is shown as a shortest fixed explicit-metre target input and parsed
on submission. It is not a fabricated receipt for the original numeric field.
Unedited placement stays
at the original position. Complete capture compares independent replay,
including property/extension JSON representations. Ordinary/proposed commits
admit the complete final map and affected joins; baseline conversion derives
distinct proposed owners while retaining originals and other alternatives.
This source implementation has not been compiled or exercised.

## Baseline-preserving roof demolition and surviving joins (v97)

Native format 97 and JSON/assets extraction version 95 retain roof demolition
under the exclusive, source-bound phase authoring intent four. Roof replacement
record four has exactly `version`, `registry_id`, `alternative_id`,
`seed_roof_ids`, `identities`, `demolition` and
`demolition_additional_identities`. `demolition` is true; profile, opening and
pose edit payloads cannot accompany this dialect. Earlier roof records retain
their existing fields and floors.

Only explicitly selected, active shared-baseline roof IDs confer demolition
authority. Replay derives their complete joined cohort from the actual source
and named saved active alternative. Every original roof and join remains exact.
Selected roofs have no proposed copy. Surviving neighbors retain their physical
geometry and context, with fresh copied child identities and source-derived
effective material assignments.
The named alternative marks the original cohort demolished and registers only
the owners actually created for its proposed state.

Surviving join members are partitioned using the same physical contact semantics
as native roof joining. Each connected component with at least two members gets
a join, preserving original authored member order and join material assignment.
Singletons remain independent roofs and inherit an admitted source join's
effective material assignment, preserving compatible raw assignment extras.
The primary mapping covers every original
owner, child and bound overlay, even when its copy is omitted. Additional join
and overlay destinations are declared as arrays keyed by their actual original
identities. Exact required counts are independently derived from source;
arbitrary keys, missing/extra slots and collisions refuse. Every destination
remains reserved through retained history, including after Undo.

Qualified coordinated views, appearances, bound overlays and annotation
overrides are extended for all actual copies while retaining their original
rows and opaque data. Final resolved roofs and joins are admitted against the
complete resulting map. Revision, full snapshot/history, saved marker and
actual phase choices retain their existing authority fences. Cut/Delete clear
the selection rather than selecting an omitted copy. Unsupported affected
references remain explicit implementation
gaps. This source contract has not been compiled or exercised.

## Atomic proposed roof envelope, opening and placement edits (v96)

Native format 96 and JSON/assets extraction version 94 retain roof replacement
record three under the same exclusive phase intent four authority. Record three
has exactly record two's seven fields plus `roof_edits`. Its historical
`roof_profiles` and `roof_opening_edits` arrays are empty; `roof_edits` is
nonempty. Earlier records keep their original fields, interpretation and floors.

Each combined roof intent is strict version one with exactly `version`,
`roof_id`, `profile`, `openings` and `pose`. Components are null when absent,
and at least one is supplied. All component owner IDs must match. Profile and
opening components retain their existing exact quantity contracts. A pose
component has exactly `version`, `roof_id`, `x`, `y`, `z` and
`orientation_radians`. Coordinates are signed/zero exact length quantities;
orientation is a finite angle scalar and has no length receipt.

Every component stages its schema-owned changes against the same admitted
original entity. The composed result owns disjoint profile, roster, pose and
quantity receipt deltas. Only the complete resulting roof must fit its final
envelope, allowing a shrink with opening removal or an expansion with a new
opening in one operation. No intermediate entity or fabricated snapshot gains
source authority. Complete final equality against independent typed replay
rejects accompanying metadata, form or unsupported receipt changes.

The actual full map admits source and final resolved roofs and affected active
fused joins. A replacement derives the complete baseline cohort, requires
exactly its actually changed seed roofs, and retains every original owner.
Fresh copied/new child identities and additive presentation references retain
record two's current/history protections. Equivalent inputs return the original
without adding history. The format floor follows retained nested and composed
proofs even after Undo. Roof form conversion, clone/delete alternatives and
unsupported affected reference families require further typed contracts.

## Baseline-preserving roof-opening edits (v95)

Native format 95 and JSON/assets extraction version 93 retain roof-opening
add, edit and remove operations inside the exclusive phase intent four roof
replacement. The floor follows every retained direct or composed proof,
including commands later undone. Earlier profile-only replacement records keep
their version-one meaning and floor.

Roof replacement record two has exactly `version`, `registry_id`,
`alternative_id`, `seed_roof_ids`, `identities`, `roof_profiles` and
`roof_opening_edits`. The profile array is empty in this dialect. Each opening
intent has exactly `version`, `roof_id`, `upserts` and `removed_opening_ids`.
An upsert has exactly `opening_id`, `x`, `y`, `width` and `depth`; null retains
an existing scalar. A new child requires all four inputs. Each nonnull input
contains exactly a strict quantity receipt and its actual `default_unit`.
Coordinates can be signed or zero; width and depth are positive.

Replay owns the declared roster changes only. It preserves the roof's pose,
profile, pitch, context, materials and retained child metadata. Known indexed
quantity receipts follow their original child identities when indices shift;
future or opaque affected bindings refuse. Changed receipt cores preserve
opaque siblings. Equivalent input retains the original receipt and numeric
representation. Capture requires exact entered receipts and complete equality
with independently replayed source data; numeric-only candidates are refused.

The complete source-derived joined roof cohort receives distinct proposed
owners. Existing children follow their declared fresh mapping; newly authored
opening identities remain explicit and disjoint from that mapping. Current and
retained source data, declared earlier intents, entity types and envelope keys
reserve these names after Undo. Original roofs, joins and other alternatives
remain exact. Source, edited and copied roofs and joins pass resolved native
admission before publication. Mixed profile/opening and form/pose edits still
require a further typed contract and cannot mutate the shared baseline.

## Baseline-preserving proposed roof profiles (v94)

Native format 94 and JSON/assets extraction version 92 retain roof replacements
under exclusive command envelope 34 and phase authoring intent version four.
Earlier phase intent versions and roof entity schemas keep their previous wire
meaning. The floor follows retained direct and composed proofs, including Undo.

Version four adds only `roof_replacement` to the source-bound phase envelope.
It cannot accompany wall replacement, opening demolition, geometry targets,
relationships, raw entity payloads or supplemental edit authority. Source
revision, snapshot/history, authoring digest, entity digest, saved marker and
actual saved phase selections bind the same captured document.

The replacement record is version one with exactly `version`, `registry_id`,
`alternative_id`, `seed_roof_ids`, `identities` and `roof_profiles`. Each profile
has exactly `version`, `roof_id`, `length`, `span`, `rise`, `overhang` and
`thickness`; absent dimensions are null. Length represents the horizontal run
of a sloped panel or plan length of a gable/hip roof. Quantities retain exact
entered expressions and units. Dependent pitch is reconstructed from the
resulting rise and run or half-span. Equal values preserve source data exactly.

Replay derives the complete joined roof cohort from the full actual map and
requires baseline membership in the named saved active alternative. Fresh,
injective identities cover roofs, joins, owned roof openings and copied bound
view overlays. Existing roofs and joins remain exact; only the named registry
and qualified additive presentation consumers change in place. Other
alternatives, view definitions, sheets, viewport links, scales and unrelated
rows remain retained. Roof-opening receipt keys follow their copied children;
opaque data is preserved. Retained history continues to reserve all fresh
identities after Undo.

Same-form profile changes are supported by this dialect. Form/pose changes,
opening-roster edits and unsupported affected references require further typed
contracts; they cannot fall through to a baseline-changing ordinary edit.
Native roof/join admission is derived from resolved physical placement and the
shared roof codec, rather than persisted BRep geometry or caller candidates.

## Exact wall top-rise and reviewed profile quantities (v93)

Native format 93 and JSON/assets extraction version 91 retain signed wall
top-rise edits and exact quantity changes inside an ordinary reviewed wall
profile. The wall replacement envelope retains record two's fields. A nested
wall profile with `top_rise` uses strict version two: the five version-one
fields plus a nonnull exact `top_rise` quantity. Profiles without top rise
retain their version-one encoding. Positive and negative rises are supported;
zero clears a supported nonflat plane. Equal effective profiles keep exact
source data and add no event. Unknown planes or dangling plane receipts refuse.

Replay retains baseline, elevation, context, layer identities/order/materials
and opaque data. An entered rise derives the top along the captured chord,
anchored at the resulting start height. Source and final active cuts,
assemblies, operations and joins pass full physical admission. Shared baseline
walls receive separate proposed copies before ordinary transactions or preview;
rooms and copied relationships still require the combined review.

Raw profile capture independently replays dimensions, complete layer thickness
inventories and top rise. It preserves entered expressions and retained opaque
receipt fields, rejects unsupported accompanying changes, and normalizes only
admitted source-equivalent encodings. Ordinary room review accepts a receipt
delta only when its wall is exactly the independently replayed typed result.
Existing no-delta proofs retain their earlier meaning and guards.
Recorded version-one physical replay keeps its earlier admission policy.
Version two and current authoring opt into complete known receipt-to-field
binding checks for every affected join member and copied wall, including
unchanged profiles. Alias fields cannot legitimize dangling canonical receipts.

Reader floors inspect nested profile versions and actual retained parent/child
wall receipts in reviewed profile history, including wrapped records and Undo.
Old proposed copies are not upgraded merely for carrying their earlier exact
quantities. Source integration has not been compiled or run in the installed app.

## Proposed opening type conversion (v92)

Native format 92 and JSON/assets extraction version 90 add wall replacement
record five inside source-bound phase intent two/envelope 34. Records one
through four retain their exact keys and replay. Record five adds only nonempty
`opening_families` to the seven common fields. Each strict version-one row names
`opening_id`, `wall_id`, `target_family`, explicit final `assembly` and final
`door_operation`. The target is `door`, `window` or `opening`. Door/window need
a matching assembly; a bare opening has none. A door operation is permitted
only for Door, and absence remains distinct from an authored default operation.

Conversion retains the actual host, scalar and exact dimension representations,
quantities, context, layer and opaque owner data. Only recognized family,
assembly and operation fields change. Both source and final profiles pass the
existing complete profile admission. Source-equivalent profiles preserve the
exact original; other same-family profile changes use the earlier profile API.
Unknown or inconsistent source definitions refuse rather than lose content.

Actual shared baseline hosts independently determine exact replacement seeds
before native editing, allocation or Document preview. Each proposed row needs
its own qualified host and actual registry membership. Opening and host map to
separate derived copies. Original and copied physical closures pass complete
cut/assembly/operation/join admission; final rooms and copied relationships still
require combined review. Baseline owners, other alternatives and unrelated
registries remain intact. Foreign known ownership and unsupported baseline-host
arrangements refuse, including through ordinary replay.

Desktop capture compares raw edits with independent entity replay and rejects
accompanying geometry, host, dimension, layer or other property changes. The
floating Type control supplies an explicit default profile for a newly chosen
family and retains the complete current profile when its type is unchanged.
Source/selection/workspace/recovery fences protect final review and publication.
Reader floors cover retained direct/wrapped records after Undo. This source has
not been compiled or qualified in the installed app.

## Proposed door and window rehosting (v91)

Native format 91 and JSON/assets extraction version 89 add wall replacement
record four inside source-bound phase intent two/envelope 34. Records one
through three keep their exact keys and replay. Record four adds only nonempty
`opening_rehosts` to the seven common fields. Each strict version-one rehost
names the original `opening_id`, `original_wall_id`, `target_wall_id` and exact
`offset` quantity. Geometry, relationship and other profile operations cannot
borrow this authority.

Preflight and replay independently derive the union of actual shared baseline
source and target host seeds in one saved active alternative. Each rehost must
qualify through its own source or target host; unrelated seed plans and foreign
registered owners refuse. All three identities map independently through the
derived complete fresh closure. Retained baseline openings require separate
copies; safe proposed and nonshared owners retain their actual identities.
The common source-bound room and copied relationship review remains mandatory.
Original baseline records and other alternatives remain exact.

Detached replay admits both original and final active host graphs, their sibling
cuts, manufactured assemblies and joins, including copied walls outside the
edited roster. Opening family, dimensions, assembly, operation, drawing layer
and opaque data remain unchanged. Only host, station and its receipt can change.
The opening and both hosts need the same resolved property/building/floor/level
and effective base elevation. Implicit context drift, cross-floor moves and
unsupported ownership refuse. Real rehosting retains the authored station
receipt even at an equal numeric value; same-host/same-station is an exact no-op.

Desktop raw capture restores only the original host for independent existing
profile admission, then allows station-only changes. Qualified copies are
discovered before an original transaction or Document preview. Unchanged station
uses retained exact quantity authority or a bounded exact numeric fallback,
never the rounded form text. Floating host-wall choices and modal room review
bind actual source, selection, saved/recovery state and workspace. Cancel or
stale capture publishes nothing. Reader floors cover retained direct/wrapped
records after Undo. This source has not been compiled or qualified in the app.

## Saved-active opening demolition (v90)

Native format 90 and JSON/assets extraction version 88 add phase intent three
within exclusive envelope 34. Versions one and two retain their exact keys and
replay. The new intent adds only `opening_demolition`: a strict version-one
record with `registry_id`, `alternative_id` and sorted, unique `opening_ids`.
It shares the captured revision, complete snapshot, authoring-history and entity
digests, saved revision and independently evaluated saved phase choices. Its
semantic intent cannot contain geometry, relationship or replacement operations.

Replay independently derives active baseline membership and changes only the
named alternative's demolition list. Opening owners, host walls, dimensions,
other dependents, proposals, other alternatives and unknown registry content
remain exact. Mixed baseline/nonbaseline selections and cross-registry batches
refuse. Source and candidate active hosts both pass the common cut, assembly and
join admission before publication. No filtered or synthesized snapshot grants
authority. A canonical command establishes saved-active validation even when
the source's earlier history used legacy all-object validation; historical
commands retain their own policies.

The active physical cut inventory applies to plan geometry, 3D preparation,
coordinate dimensions, opening edits, IFC/DXF output and material quantities.
Inactive opening records and bindings stay in the project for baseline and other
alternative views. Individual retained descriptors still receive structural
admission; inactive sibling cuts do not participate in active-host fit/overlap.
Exporters explicitly report omitted phase evidence rather than claim phase
round-trip fidelity. Reader floors include retained direct and wrapped intents
after Undo. This source has not been compiled or qualified in the installed app.

## Proposed door and window profile edits (v89)

Native format 89 and JSON/assets extraction version 87 add version-three wall
replacement records within source-bound envelope 34/phase intent two. The record
adds a nonempty `opening_profiles` array; earlier record dialects keep their exact
keys and behavior. Wall profiles and geometric/relationship edits remain separate
reviewed operations. Each opening profile retains its original `opening_id` and
`wall_id`, exact optional `offset`, `width`, `sill` and `height` quantity receipts,
optional same-family `assembly` and `door_operation`, and an explicit
`clear_door_operation` boolean. Null means unchanged; setting and clearing an
operation together refuses. An absent door operation has no inferred swing and
is distinct from an authored default hinged operation.

Profiles target existing openings on explicitly seeded shared baseline hosts.
Qualified opening and wall identities map to separate proposed copies before
physical replay. Complete host cuts, manufactured assemblies, resolved placement
and affected active joins use the common physical factories. Baseline owners and
their opaque fields remain intact. Unrelated unhosted schedule/transport openings
do not participate in affected-host admission. Rooms remain original until the
same mandatory combined review completes their definitions and copied constraint
decisions; final active constraint admission still applies.

Desktop capture independently compares raw edits with typed replay, retaining
entered quantities and unknown receipt fields. Stale known receipts, unexpected
field changes, family changes, rehosting and unsupported interior extensions
refuse. Canvas jamb resizing and station sliding discover proposals before an
ordinary original-object command is prepared. Detached previews retain actual
fresh physical identities, captured label presentation and original canvas
aliases. Release consumes the exact displayed typed intent, retires gesture
state and retains source, selection, save/recovery and viewport fences throughout
room review. Cancel publishes nothing. Generic prepared publication cannot
publish an unfinished physical stage.

Reader floors include all retained direct and wrapped version-three intents,
including after Undo. Broader opening creation/deletion/rehosting and compound
multi-registry edits require their own completion paths. This source has not
been compiled or qualified in the installed application.

## Proposed wall height and depth edits (v88)

Native format 88 and JSON/assets extraction version 86 add version-two wall
replacement records inside the existing source-bound envelope 34/phase intent
two. The record adds a nonempty `wall_profiles` array; version-one records keep
their exact keys and replay. Each strict version-one profile has `wall_id`,
`thickness`, `height` and `layer_thicknesses`, with explicit null optional values
and exact quantity receipts. A supplied layer roster names every retained layer
in its original order; only its thickness can change.

Profiles target explicitly seeded original walls and independently map to the
qualified proposed wall and layer identities. Coordinates, sweep, context,
elevation, top plane, layer materials and opaque data remain intact. Recognized
quantity entries preserve their unknown fields; unsupported or stale entries
refuse. Hosted opening dimensions, assemblies and affected active fused joins
use the same physical factories as architectural authoring, with resolved walls
and their complete cuts; derived solids are discarded. Active exterior
measurement consequences are reconstructed with stable topology.
Physical rooms retain their original data until the same mandatory room review
completes the edit. Final active constraint admission remains required.

Profile-only operations can skip the geometry solver only when a nonempty typed
profile has been admitted. This grants no raw wall payload or other-property
authority. Desktop conversion compares captured height/depth/layer changes to
typed entity replay before the original property transaction, augmentation or
preview. Entered quantity expressions remain exact; raw numeric capture uses a
bounded decimal that reparses to the same stored value. Cancel and stale capture
refuse publication before entering ordinary room wrappers.
Multi-registry, mixed-object and broader property/lifecycle changes need their
own completion paths. Reader floors cover retained and composed intents after
Undo. This source has not been compiled or qualified in the installed app.

Canvas preview preparation also uses the source-bound replacement intent before
the original geometry solve. Its detached physical map is an ephemeral value,
not a DocumentSnapshot or history entry. Presentation aliases use captured
original IDs while the real copied entity/child IDs remain fresh. Generic edit
publication refuses unfinished room stages; release must complete the same typed
intent and full source/recovery/view fences before ordinary final admission.
Preview values never certify unreviewed room or appraisal calculations.

## Proposed replacements for shared baseline walls (v87)

Native format 87 and JSON/assets extraction version 85 retain envelope 34 with
version-two `phase_constraint_authoring_intent`. Version one keeps its exact
keys and replay. Version two adds `wall_replacement`; its strict version-one
record contains `registry_id`, `alternative_id`, sorted `seed_wall_ids`, a
complete `identities` map, `room_review_intent`, and
`room_constraint_decisions`. Geometry is independently reconstructed rather
than supplied as a replacement payload.

Semantic root discovery precedes solving original geometry. The actual saved
alternative, baseline membership, physical contacts, typed relationships,
hosted openings, joins and supported measurement source cohorts determine the
required replacement graph. Each copied owner and owned child receives a fresh
identity. The named alternative marks original baseline model members demolished
and adds the independent replacements as proposals. All original objects,
unrelated registry records and other alternatives remain intact. Retained
history reserves declared copy identities even when an explicitly omitted new
relationship never appears in the final entity map.

Physical-room owners are not copied from stale outlines. The full detached
physical stage supplies mandatory room correspondence and incoming-reference
evidence. The room-review child binds this independently reconstructed entity
map while its snapshot, authoring history, saved revision and expected revision
still identify the complete original capture. A required room child cannot be
omitted, and its registry command cannot alter the derived replacement registry.

Copied relationships with physical-room endpoints are withheld from the
physical solve and included in room review evidence. Every copy requires an
explicit Keep, Remap or Omit decision. Remapping names each original room
binding index and a reviewed active room's stable edge, vertex and endpoint
role in the same drawing context and physical plane. Removal intent requires
Omit. Transient room completion may defer only these independently reconstructed
fresh copies; original relationships and final active residual validation are
preserved. The complete wall and room edit commits as one reversible event.

Selection composition first validates the exact geometry child, then its
separately admitted ordinary edits and final reviewed room topology/lineage.
DISTO attachment retains the original source owner in its observation intent;
only a canonical replacement map can redirect the completed target to its new
proposed identity. The original observation metadata stays intact.

Unknown affected references, stale source evidence and unsupported retained
proofs refuse without partial changes. Physical-room split/merge repair proofs
and replacement across several registries need additional lifecycle support.
Reader floors cover all retained direct and composed proofs, including after
Undo. This source has not been compiled or qualified in the installed app.

## Active-design constraint authoring (v86)

Native format 86 and JSON/assets extraction version 84 retain exclusive
`apply_boundary_constraint_changes` envelope 34. Its version-one
`phase_constraint_authoring_intent` binds the actual original revision, complete
snapshot, authoring history, saved revision, entity map and independently
evaluated saved choices for every admitted phase registry. The normalized
semantic intent carries explicit geometry targets, exact quantities, anchors,
movement choices, rigid operators and relation mutations. Raw entity/asset
payloads and competing completion fields cannot grant this authority.

Every recognized constraint and stable binding is structurally admitted across
the complete map. Genuine curve/type requirements remain global. Only residual
satisfaction and solver participation are suspended when a known relation binds
an inactive owner. Unknown semantics retain the read-only diagnostic. Contact,
topology, source redraw and component freedom use actual saved-active scope;
inactive owners, their saved constraints, hosted openings, measurements and
annotation placements remain intact. Qualified active axis-lock changes under
captured quarter-turn rigid operators are independently reconstructed.

Live edits and retained replay rederive the complete result with the same
entity-only active builder. This creates no filtered or fabricated source
snapshot. Reviewed rooms can follow the canonical geometry proof in the
existing outer room envelope; selection and DISTO wrappers retain their own
qualified suffixes and the original source binding. New identities cannot reuse
retained entity or recognized boundary/linework child identities.

State validation policy follows actual command/navigation ancestry. A canonical
34 command establishes active semantics, subsequent events inherit them, and
Undo/Redo restore the referenced event's policy. Registry metadata alone never
reinterprets a historical state. Legacy builders and historical helpers retain
their collect-all behavior. The minimum reader covers all retained proofs,
including wrapped, undone and abandoned edits; old floors remain unchanged for
projects without the new dialect. This source has not been compiled or qualified
in the installed app.

## Reviewed proposed-room editing (v85)

Native format 85 and JSON/assets extraction version 83 retain version-two
`phase_room_review_intent` beneath exclusive envelope 33. The required
`proposed_room_completion: true` marker adds actual target-proposal redefinition
and retirement while version-one keys and replay remain unchanged.

Redefinition preserves the owner's identity, authored facts and opaque metadata,
and derives fresh topology and source evidence from the explicitly evaluated
target design. Retirement removes only actual proposals in that same target.
Every affected supported dimension or constraint has an explicit Keep/Remove
decision; retained children use reviewed mappings, automatic edge dimensions
use fresh replacement identities, and removed relationship rows carry exact
acknowledgements. Unsupported incoming identity references refuse. Baseline
room entities and unrelated alternatives remain exact.

Version two additionally requires canonical `presentation_removals` evidence
for every affected saved sheet or annotation record. Each row binds the original
and independently derived replacement singleton digests plus its exact removed
tokens. Reviewing these removals cannot change baseline-only analytical
dependents or baseline-referencing presentation rows and restrictions. Opaque
identity mentions in a deleted presentation row are checked before pruning.

Replay independently reconstructs the entire admitted entity map. Source-bound
room transitions compare that reconstruction with the published result. Fresh
children and replacement dimension identities cannot reuse any retained-history
owner or recognized boundary/linework child. The floor applies to every retained
event, including undone or abandoned edits. Earlier version-one phase reviews
and ordinary active-phase intent 3 retain their native 84/extraction 82 floor.
This source has not been compiled or qualified in the installed app.

## Baseline-preserving room alternatives (v84)

Native format 84 and JSON/assets extraction version 82 retain exclusive
`apply_boundary_constraint_changes` envelope 33. Its version-one
`phase_room_review_intent` binds the complete original snapshot, authoring
history, saved revision, entity map and registry entity. The canonical child
upserts only the named phase registry. Replay independently derives explicit
phase inventories, required context/planes, original-room coverage and current
clear spaces before applying reviewed assignments.

Unchanged rooms share original identities and facts. Superseded baseline rooms
and children remain intact; only their target-alternative demolition membership
changes. Proposed rooms receive fresh owner/edge/vertex identities, actual
detected outlines/holes/source lineage and explicitly reviewed name,
classification and factor. Unclassified candidates remain unassigned. Incoming
saved references, including opaque identity mentions, require complete exact
baseline-preservation acknowledgements. No reference copy or retargeting is
implied. Version one cannot redefine or retire existing proposed rooms; version
two provides the separate explicit editing authority described above.

Competing geometry, raw-entity, asset and ordinary room-review authorities refuse.
Fresh identities cannot reuse retained-history owners or recognized boundary and
linework children. Requested planes cannot claim the same actual supporting
component twice. The floor covers all retained history, including after Undo,
deletion or abandoned edits. Terrain membership in a phase registry also requires
format 84. Earlier projects keep their existing minimum reader.

This source batch has not been compiled or qualified in the installed app.

Ordinary physical-room intent version 3 also requires reader 84/extraction 82.
Its explicit `active_phase_room_scope: true` and `context_plane_selection`
boolean retain the exact saved phase choices through the original entity and
snapshot bindings. Correspondence and replay derive the active owner roster from
actual admitted registries; inactive baseline and other-alternative rooms keep
their original payloads. Fresh rooms join the one registry resolved by actual
clear-boundary wall support, or its real selected-wall fallback, in the saved
alternative or baseline. Conflicting supporting registries refuse. Older intent
versions 1 and 2 retain their original keys and collect-all replay semantics.

## Joint wall movement and room review (v83)

Native format 83 and JSON/assets extraction version 81 retain intact joint-wall
envelope 17 beneath a physical-room review. A single room-context completion
uses envelope 32; multi-context completion retains envelope 27. The floor applies
throughout history, including after Undo or later edits. Existing direct rigid
envelope 28 and grouped deletion envelope 31 keep their separate meanings.

The child retains the original `JointTranslationIntent`, all selected source
identities and every saved translation or rigid operator. Its ordinary admission
independently reconstructs connected geometry, constraints, source-derived
measurements, saved callouts and qualified presentation placements before room
decisions are replayed. The room wrapper does not unwrap the joint proof, replace
per-owner operators with a common transform or supply additional raw geometry.
Every affected original room across contexts and effective planes remains
explicitly covered in one final event.

Eligibility requires physical-wall edit proofs, explicit joint intent/completion,
selected partial walls, canonical version 17 and the existing 1 MiB event budget.
Competing room/group/selection/specialized wrappers and actual asset mutation or
asset-reference completion refuse. Genuine lower dimension, measured/exterior
and position-only presentation receipts keep their original child meanings.

This is a source contract. Compilation, interaction and persistence qualification
remain pending.

## Grouped wall deletion with reviewed rooms (v82)

Native format 82 and JSON/assets extraction version 80 retain grouped wall
deletion reviews throughout history. A single-context completion uses envelope
31; a multi-context completion keeps envelope 27 with its grouped child proof.
The child has exactly `version`, `kind`, `expected_revision`, `message`,
`wall_ids` and `proof`. Its version is 31 and its kind is
`physical_wall_deletion`; `wall_ids` contains two to 128 sorted, unique original
wall identities. `proof` retains the canonical ordinary version-one command.
The complete room event remains bounded to 1 MiB.

Decoding checks declared erasures and the exact child revision/message.
Admission requires that the declared inventory equals every original wall
erased by the child. It independently reconstructs hosted openings, supported
dimensions and constraints, known memberships and opaque-reference refusal
from those original walls. Undeclared walls, unrelated changes, replacement
walls and asset lanes refuse. Single-wall envelope 30 retains its one-wall
meaning and does not acquire grouped authority.

The desktop route captures an entirely physical-wall selection and reviews all
affected original room contexts/planes before a single publication. Cancelling
discards every staged room decision. Cut publishes its clipboard payload only
after the complete drawing change succeeds. Mixed selections, embedded
assemblies and phase demolition retain their separate routes.

This is a source contract; compilation and runtime qualification remain pending.

## Wall deletion and context-based room review (v81)

Native format 81 and JSON/assets extraction version 79 retain version-two room
review intents. Their existing exact fieldset now has an empty selected-wall
identity and an explicit drawing context/elevation plane. Discovery uses actual
active walls in that plane; an empty wall set produces an empty result without
a fabricated source. Room-only envelope 29, single deletion envelope 30, and
batch envelope 27 with version-two intents require this reader floor throughout
retained history. Earlier intent/envelope meanings remain unchanged.

A deletion child is the exact canonical ordinary command for one original wall,
its supported hosted openings, saved dimensions and attached constraints, plus
known phase/view/presentation membership cleanup. Source admission reconstructs
that command independently and rejects unrelated changes. Room owners and their
facts remain original until explicit dispositions are replayed. Every affected
retained room must be covered, including across contexts and physical planes.
The actual original source and all staged entity-map/history/save fences remain
bound to one final event.

Assigned fresh spaces require a live admitted wall that independently reproduces
the exact reviewed source lineage. Retained identities keep their own metadata;
new identities require explicit name/classification and an interior witness.
Explicit retirement also removes known phase memberships, saved-view object
references, associated view overlays/appearance and presentation overrides.
Opaque metadata and unrelated registrations remain intact; unknown incoming
references continue to refuse. Exterior appraisal observations retain their
historical evidence and withhold quantities when their physical source is lost.

This is a source contract. Compilation, runtime interactions and persistence
round trips have not been qualified for this batch.

## Reviewed rigid wall and room changes (v80)

Native format 80 and JSON/assets extraction version 78 retain single-review
boundary envelope 28, or multi-context envelope 27 containing a qualified rigid
child. The original full source fences and sequential entity-map fences retain
their existing meanings. The underlying direct command is rigid envelope 10,
mixed measured-source envelope 11 with rigid walls, or the narrow wall-callout
completion 21 wrapping 10/11. At least one curved v4 or straight v5 wall proof
must carry its genuine rigid operator. Ordinary v1/v2/v3 connected neighbors
retain their existing authority; measured-only commands cannot enter this lane.

Canonical replay validates the existing rigid geometry, hosted consequences,
source measurements, constraints and saved callouts before room review receives
authority. The wall-callout child retains its existing shared-operator contract.
Room/selection/DISTO/joint/group/split/merge/corner/resize/arc/dimension-placement
intents, curve construction, asset changes/references and arbitrary wrappers
cannot borrow this completion. Physical-wall identity inventory remains exact.
Final room replay and identity checks follow the admitted candidate in one event.

The native reader floor is retained for these child proofs throughout all
history, including within multi-context review. The ordinary envelope 25 and
curve/profile envelopes 24/26 keep their original child contracts.

## Reviewed rooms across contexts and planes (v79)

Native format 79 and JSON/assets extraction version 77 retain
boundary-constraint envelope 27. One admitted original physical-wall geometry
or profile proof follows the same bounded child contracts as envelopes 24/25/26.
The first explicit `room_review_intent` is followed by
`room_review_additional_intents`, with a true `room_review_batch_completion`
marker and two to thirty-two total decisions. Raw top-level edit lanes,
recursive wrappers, stripped markers and missing or oversized batches refuse.
The complete event remains bounded to 1 MiB.

Each context/effective-plane group is reviewed against the cumulative detached
entity map produced by the previous accepted group. Its entity-map digest stays
bound to that actual stage. Every full snapshot, authoring-history and save fence
binds the same original document. Replay derives the child once, then sequentially
reconstructs every explicit room decision. Every affected retained room must be
covered. Duplicate contexts/planes, overlapping retained identities and fresh
identity reuse across original history or intermediate stages refuse, including
retired room, boundary-child and replaced dimension identities.

No intermediate geometry or room event is published. Cancelling any group
discards every detached decision; accepting publishes one event and one Undo
restores the complete original state. Native history, including undone and
abandoned branches, retains the new reader floor. Single-review envelopes
18/24/25/26 preserve their previous meanings and wire shape.

## Reviewed wall-profile and room changes (v78)

Native format 78 and JSON/assets extraction version 76 retain
boundary-constraint envelope 26. It uses the same original/derived source
fences as envelopes 24/25, with one profile-only child. That child is either
ordinary entity envelope 1 with one existing wall upsert and no assets, or
boundary envelope 6/7 with one physical-wall upsert and qualified exterior
measurement redraws. No baseline edit is invented to represent a profile.

Only `height_m`, `height`, `thickness_m`, `thickness`, `layers`, `top_plane`,
`slope_rise_m` and `slope_rise` may differ
from the captured wall. Identity, required state, extensions, baseline, context
and every other property remain exact. Other walls remain unchanged; new,
removed or replaced physical source walls refuse. Raw profile children cannot
leave linked exterior measurements stale. Completed children independently
rederive their exact exterior redraws and full candidate before room authority
is granted. The v7 child permits its source-completion flag for declared sloped
top fields, while its supplemental entity/asset lanes remain empty. Asset or
unrelated payload lanes cannot supply this completion.

Admission replays the profile first, then the explicit room decisions against
that exact result. Height/thickness/slope/layer changes, exterior measurements and
rooms publish in one event. Undo restores all of them together. Mixed profile
and geometry/metadata edits cannot borrow profile-only review authority.
The declared outer dialect must agree with its child. The new reader floor
applies to every retained revision, including undone and abandoned changes.

## Reviewed ordinary physical-wall and room changes (v77)

Native format 77 and JSON/assets extraction version 75 retain
boundary-constraint envelope 25. Its fields and original/derived source fences
follow envelope 24 below, but its direct geometry proof contains ordinary
physical-wall endpoint or length edits. The admitted child dialects are
2/3/4/5/6/7/11 with nonempty wall proofs of versions 1/2/3. Connected boundary,
measured-source and source-owned annotation consequences keep their existing
child authority. Specialized geometric intents, recursive wrappers and asset
change/reference lanes cannot enter this completion.

Admission independently reconstructs the child's ordinary consequences and
checks source-bound room outlines before granting explicit reviewed room
authority. The child assets and physical-wall identity inventory remain
unchanged: this completion cannot add, remove or replace source walls.
Changed-wall coverage compares exact wall payloads, including thickness,
height, layers and retained source evidence, rather than only baselines. It
then rederives the room
decisions on that exact map and checks all affected retained-room coverage and
final identity lifetimes. No intermediate wall event is published. One Undo
restores the wall, related geometry and reviewed rooms together.

Envelope 24 continues to require a direct curve proof 23. A 24/25 discriminator
that disagrees with its child refuses instead of borrowing the other dialect.
The new native reader floor applies throughout retained history, including
undone and abandoned ordinary-wall reviews.

## Reviewed curve and physical-room changes (v76)

Native format 76 and JSON/assets extraction version 74 retain
boundary-constraint envelope 24. It contains true `room_review_completion` and
`room_review_geometry_completion` markers, an explicit `room_review_intent`,
and one `room_review_geometry_proof`. The geometry proof must be a direct
curve-construction envelope 23 with the same original revision and message.
Recursive room/selection wrappers, arbitrary geometry commands and raw top-level
edit lanes cannot supply this authority. The complete proof is bounded to 1 MiB.

Preparation first admits the proposed curve on a detached source, then presents
physical-room correspondence against that geometry. Retained/new/retired room
identities, classifications, interior witnesses, child mappings, dimensions,
constraints and relationship removals remain explicit reviewed decisions.
Preparation publishes neither the intermediate curve nor the rooms.

The retained room intent's full snapshot, authoring-history and save hashes bind
the original source. Its entity-map digest binds the independently replayed
curve candidate that produced the room report. This distinction permits one
atomic event without retaining a fictitious intermediate history event or
recursively forking history during load. Admission replays the curve first,
rederives the room decisions on its resulting map, checks final identity
lifetimes and validates the full state. Undo restores both together.

Room-only envelope 18 and curve-only envelope 23 retain their meanings. Either
new marker or geometry-proof presence anywhere in retained history raises the
reader floor, including undone and abandoned revisions. Removing the curve proof
cannot reinterpret its reviewed entity map as an ordinary room-only source.

## Explicit connected curve construction (v75)

Native format 75 and JSON/assets extraction version 73 retain
boundary-constraint envelope 23. Its true `curve_construction_completion`
marker wraps one bounded endpoint/source proof and survives optional outer
selection completion 22. The wrapped command retains the same revision and
message. Missing or contradictory mode markers cannot acquire construction
authority during document admission.

Wall proof version 6 records `wall_id`, the exact proposed `baseline`, a null
`length_entry`, version-2 `curve_construction`, and nullable
`wall_classification`. It reconstructs an existing curved wall from its chord
endpoints and entered angle, signed arc length or signed height. Entered and
normalized expressions must reproduce the retained value and signed sweep.
Opaque construction fields retain their original values; unrelated wall
properties remain source-owned. Changed geometry invalidates only understood
physical-length receipts. Construction changes append to the original archive;
an exact unchanged construction does not append a duplicate operation.

The selected construction is prepared before persistent tangent relations
are converted into solver requests. Both selected endpoints are pinned to the
request. Connected neighbors retain their signed sweeps and solve through their
endpoints, subject to fixed anchors, original physical contacts, topology and
hosted-opening checks. Holding connected walls fixed refuses incompatible
requests rather than breaking their relationships. Source-derived measurements
are reconstructed through their existing qualified source lanes. A physical
room that requires a separate correspondence decision still requires explicit
repair.

Document admission independently replays every version-6 wall from the original
entity and compares its complete result. Only those verified owner IDs receive
explicit construction authority; other walls keep the preceding endpoint-only
rules. The marker or version-6 fields anywhere in retained history raise the
reader floor, including undone and abandoned revisions. Older proof versions
retain their existing meanings.

## Connected rigid owner edits (v74)

Native format 74 and JSON/assets extraction version 72 retain nested joint
intent version 4 inside boundary-constraint envelope 17 and its optional
selection-completion envelope 22. The existing selection arrays identify rigid
boundaries, measured strokes and physical walls; `owner_transformations`
contains exactly one bounded saved-coordinate operator for every selected
geometric owner. `per_owner_rigid_completion` is true, even if malformed input
removes the operators. Geometry never borrows the displayed `offset`, which
may be zero for a rotation. Version 4 excludes `owner_translations`; independent
dimension and presentation targets retain their explicit translation semantics.
The existing aggregate 4,096-target and 1 MiB proof limits apply.

One connected solve pins selected geometric points to their captured operators.
Unselected hard-related geometry can solve without joining the rigid selection.
Fixed anchors keep their source coordinates; contradictory targets refuse the
whole command. Quarter-turn axis locks reconstruct from the source. Selected
wall/stroke geometry and ordinary boundary receipts retain exact operator
replay, curve sweep, stable identities and entered measurements. Current source
cohorts and deductions remain required and compatible.

Source-bound measured areas use independently derived final wall/stroke faces.
A unique cyclic or reversed correspondence at the established machine-roundoff
bound retains their original segment/vertex identities and exact current source
lineage. These derived coordinates, rather than raw transformed coordinate bits,
are their final geometry. Unrelated or ambiguous matches refuse. A complete
physical wall cycle or cyclic branch block may invert winding only when every
member has the same independently verified reflective operator; original
endpoint/contact evidence and intersection protections remain intact.

Hosted opening reflection, wall top-plane basis and owned measurement placement
reconstruct from the original source once. Generated area callouts use their
effective source positions and final analytical anchors, including default
separated name/value offsets and text directions. Missing separated counterparts
materialize only in an existing provider. Automatic combined placement stays
automatic. Saved measurements retain automatic/manual provenance. In version 4 only,
independent annotation fields in the outer selection lane can join geometry-owned
fields in the same container. Original row identities/order and competing field
edits are checked independently. Only the reconstructed callout suffix can extend
geometry-owned rows; ordinary selection inventories stay fixed. Supported schema
floors reconcile without a downgrade. This grants no raw boundary or constraint authority.
All retained history raises the reader floor from marker or operator-list
presence. Historical joint versions and outer completion behavior stay unchanged.

## Per-owner rigid geometry groups (v73)

Native format 73 and JSON/assets extraction version 71 retain
`transform_boundaries` command dialect 3. Boundary `transformations` carry their
own saved-coordinate operators. `source_transformations` contains explicit
`owner_id` and `transform` records for every supplemental physical wall or
measured stroke. Their identities are unique and disjoint from boundary targets;
their set must exactly match the wall/stroke witnesses. The true
`per_owner_transform_completion` marker retains this dialect even with an empty
source array. At least one geometric owner is required. Boundary-only, wall-only,
stroke-only and mixed complete rigid groups use the same atomic command.
The persisted proof remains limited to 1 MiB. New owner operators bound pivot
and offset coordinates to an absolute 1e12 metres and rotation to 1e6 radians;
legacy dialect parameter admission is unchanged.

Geometry reconstructs from the preceding source and each owner's declared
operator. The first boundary never grants an implicit transform to another
owner. Deductions and retained wall/stroke source cohorts require compatible
operators and current lineage. Saved dimensions follow their validated source
owner once, retaining automatic/manual placement. Raw boundary and dimension
supplements cannot grant geometric authority. Source-derived boundaries refresh
from the resulting source geometry; untouched consumers cannot silently become
stale. Boundary replay validates the original source once for the group,
reconstructs each owner from that source and moves native saved dimensions in
one pass. It never chains moved boundaries as another owner's proof.

Hard relationships require every bound owner in the complete transformed set
with compatible operators. Existing fixed anchors keep their source coordinates;
geometry that leaves a locked point refuses. Other anchors and supported axis
locks replay from the owner operator. An external or incompatible relationship
refuses the complete event. Hosted door/window handedness and insets reconstruct
from the validated host operator even when their supplemental payload is omitted.
This command does not authorize a partial connected solve. Independently framed
ordinary architectural and presentation edits may still join through the
existing complete-candidate admission.

The floor follows the marker or retained source target list throughout all
history, including a nested rigid-group child in a mixed constraint command.
Older readers refuse the elevated floor. Dialects 1 and 2 retain their shared
operator, wire shape and replay semantics.

## Per-owner connected translations (v72)

Native format 72 and JSON/assets extraction version 70 retain nested joint
translation intent version 3 in command envelope 17, optionally inside selection
completion envelope 22. Version 3 keeps the geometry selection ID arrays and
adds `owner_translations` and `dimension_translations`. Each target contains
`owner_id` and a finite saved-coordinate `offset`. Owner targets cover exactly
the union of selected rigid boundaries, measured strokes and physical walls;
dimension targets cover exactly `dimension_ids`. The aggregate geometry and
dimension identity budget remains 4,096. A nonempty geometry selection is
required. The common `offset` records the finite nonzero displayed displacement;
it never substitutes for a missing owner offset. `presentation_offset` is null.
Version-two explicit annotation/reference targets remain available.

Each selected geometric point is pinned to its own translated source position
in one connected solve. Current source-derived boundaries retain their required
wall/stroke cohort and inherited offsets; conflicting selections refuse. Their
final redraw occurs before relation and topology admission. Curves, segment and
vertex identities, fixed anchors and persisted relations retain their existing
typed rules. Selected geometry must match exact translation replay.

Saved dimensions owned by rigidly moved geometry follow once and preserve
automatic/manual provenance. Independently selected dimension positions use
their explicit offsets and become manual. Without changed walls, version-three
callouts reconstruct through the joint intent rather than the lower wall-only
placement lane. Area label offsets and angles retain their relative source
values during translation.

Strict field, source-type, duplicate, alias and target-coverage validation applies
before reconstruction. The in-memory version-three marker retains this reader
floor even if target arrays are removed; malformed coverage refuses rather than
downgrading. Every retained revision, including undone and abandoned history,
contributes to the reader floor. Versions one and two keep their original wire
shape, mixed-selection requirement and replay order.

## Atomic geometric and architectural selection edits (v71)

Native format 71 and JSON/assets extraction version 69 retain command envelope
22. It contains `version`, `kind`, `expected_revision`, `message`,
`selection_completion`, `proof` and `selection_entity_changes`. The marker must
be true, even for an empty ordinary lane. The geometry proof is one unchanged
boundary-constraint command in dialect 1 through 21, with the same revision
and message. Completion envelopes cannot nest.

At most 1,000 unique existing same-type supported nonwall object upserts are
admitted through the ordinary lane. Creation, deletion, asset edits, measured
owners, walls and constraint changes are excluded. Embedded catalogs retain
their definitions, instance identities and host bindings; only existing root
transforms can change. Both lanes replay against the original source. Exact
equal consequences merge once, conflicting payloads refuse, and the union must
pass document, identity, room-source and constraint admission. The original
typed geometry proof retains its authority; ordinary objects gain none of it.

The reader floor covers every retained revision, including undone edits. A
wrapped geometry proof can retain compact asset references from an earlier
dialect; decoding waits until that revision's assets have been validated and
loaded. Older histories keep their earlier format floor until this command is
retained. Existing geometry command dialects remain unchanged.

## Per-target connected presentation movement (v70)

Native format 70 and JSON/assets extraction version 68 retain nested joint
translation intent version 2 in command envelope 17. The geometry selection
still has one qualified connected solve and local offset. The intent adds
`annotation_translations` (`owner_id`, `child_id`, `offset`) and
`reference_translations` (`reference_id`, `offset`), with at most 1,000 unique
targets combined. Each offset uses its target's saved coordinate frame. The
legacy `presentation_offset` must be null. An explicit in-memory completion
marker preserves version 2 even when both arrays are empty.

Envelope 17 adds `presentation_proof` for nested versions 2 and 3. Its bounded
annotation/reference owner upserts are redundant result witnesses, separate
from the ordinary geometry proof. Replay reconstructs positions from the
preceding source and requires exact full-payload agreement before admitting
the geometry and merging those reconstructed consequences. Metadata, child
order, unselected children, styles, sizes, baselines, calibration and assets
stay unchanged. No raw entity payload grants extra geometry or placement
authority. Duplicate targets, conflicting results, unknown source identities,
asset edits and coordinate overflow are refused. Version-one wire shape and
replay semantics remain unchanged.

The reader floor includes every retained revision, including undone and
abandoned commands and an empty version-two target list. Older stored projects
retain their previous floor until they contain this new intent. Site saved
dimension presentations derive their spatial basis from the analytical owner;
this is rebuilt view state, without changing stored measurements or coordinates.

## Combined live area-callout rotation (v69)

Native format 69 and JSON/assets extraction version 67 retain annotation state
version 11. Combined `target_kind: "area"` presentation records may now carry
the finite `plan_label_rotation_radians` model angle. Versions 1 through 10
reject that combined rotation field; separated name/calculation callouts retain
their version-8 contract. Model-plan symbols remain supported in versions 10
and 11. Encoding selects version 11 only when a combined rotation is present.
Known combined rotation markers and version-11 states raise the reader floor
in every retained revision, including deleted, undone and abandoned records.

Callout placement changes preserve the semantic owner and derived text. The
stored `plan_label_offset_m` is the desired absolute model position minus the
candidate owner's analytical label anchor. Appearance, visibility and unknown
record fields remain intact. Missing nonidentity providers use an isolated
annotation-only container; identity placement creates no records or upgrades.
Absent separated-role offsets retain the generated defaults `(0, .18)` and
`(0, -.18)` metres. Creating a placement provider does not introduce a paper
text height. Combined callouts without authored offsets keep automatic
collision-aware placement and default upright behavior.

## Architectural reflections (v68)

Native format 68 and JSON/assets extraction version 66 retain assembly mirror
parity and straight stair alignment. The reader floor scans every retained
revision, including deleted, undone and abandoned records. Lowering either
native format marker refuses the file instead of dropping these semantics.

Assembly root transforms, local part transforms, nested path overrides and
legacy host placements may contain the optional boolean `mirrored_y`. Missing
means false; canonical encoding omits false. The exact operator is
`translation + scale * R(yaw) * D_y(mirrored_y) * point`, with positive uniform
scale and local Y reflection before yaw. Reflection leaves local Z unchanged.
Composition reverses a child's yaw under a mirrored parent and XORs parity.
The floor follows marker presence, including explicit false, only at these
schema-owned transform paths; similarly named opaque user properties do not
raise it. Existing assembly envelope names remain unchanged.

Multi-flight stairs add properties version 4. It inherits version 3's optional
per-flight going and width. A straight connecting landing may carry
`straight_alignment: "right"`; no other value or turn is accepted. Older stair
versions reject this field. Missing means the historical near-width-edge
alignment; `"right"` aligns the far edge of the positive local-width interval.
For incoming width `wi` and outgoing width `wo`, that landing covers local Y
`[min(0, wi - wo), wi]` and the outgoing origin has Y `wi - wo`. This preserves
unequal widths under reflection. In the editor the far edge is physical Left
when looking up the stair, matching the hosted-railing side convention.

Reflection retains stable flight/landing IDs, swaps physical flight-railing
sides and remaps landing edge indices and fractions through the original edge
endpoints. Authoring, subsequent transforms and plan resizing retain a source
version-4 envelope even after its last alignment marker is cleared. IFC
original-proof comparison also recognizes version-4 child identities.

## Independent measured copies (v67)

Native format 67 and JSON/assets extraction version 65 retain
`extensions.measurement_linework_copy_scope: {"version":1}` on copied
`measurement_linework` owners. This optional semantic flag has exactly one
integer field. Wrong owner types, additional fields and other versions are
refused. The marker owns the reader floor in every retained revision, including
undone, deleted and abandoned owners; a lowered native marker is refused.

If a measured area's retained exterior or group-member lineage references any
marked owner, its source graph contains exactly every referenced owner and all
canonical edges of each owner. Source contexts, phase visibility, segment
identities, intervals, geometry and complete group topology still have to
match. The flag supplies no arbitrary scope identifier or source authority.
Unmarked lineage retains the historical layer graph, excluding marked copies.
With no markers, older commands and their replay semantics remain unchanged.
Coincident copies can therefore retain independent provenance without changing
the original owners or relying on their continued existence.

Each source-check call permits 16 resident cohort graphs, 256 reconstructions,
65,536 canonical source segments and 16,000,000 possible segment pairs.
Evictions and failed builds consume work. Existing graph and topology limits
also apply. These limits bound added graph reconstruction, not every earlier
lineage-decoding operation. Exhausted work refuses the affected check.

## Qualified measured lines and saved wall dimensions in rigid groups (v66)

Native format 66 and JSON/assets extraction version 64 retain explicit
measured-line and saved-wall-dimension completion. Earlier commands keep their
original replay behavior; reading them does not add geometry authority or
dimension movement retroactively.

The version-2 `transform_boundaries` envelope adds exactly the two boolean
fields `wall_dimension_completion` and `measured_stroke_transform_completion`
to the version-1 fields. At least one must be true, and each true field requires
an independently validated owner witness. Measured-line completion reconstructs
every supplied line from the same source rigid transform, even without saved
dimensions, before admitting its internal constraints. Wall completion
reconstructs physical geometry before moving each attached saved callout's
original text position once. Raw dimension supplements cannot supply that
authority. Automatic/manual placement provenance and unrecognized saved
presentation fields remain intact.

Constraint command envelope 21 has exactly `version`,
`kind: "apply_boundary_constraint_changes"`, `expected_revision`, `message`,
`wall_dimension_completion: true` and `proof`. The proof is one existing
version-10 rigid wall or version-11 wall/measured-line command with the same
revision and message. This wrapper does not nest or lend its authority to
unrelated completion lanes. Every selected wall and measured line must retain
the same explicit rigid transform. Named wall endpoint identities remain
stable through reflection and half-turns; ordinary reversal admission is
unchanged.

The flags require format 66 anywhere in retained history, including nested
rigid group proofs within envelope 16, undone commands and deleted owners. Older projects keep
their previous floor and command bytes. Lowered markers are refused even when
their digests have been recomputed.

## Current-room continuity during physical wall splits (v65)

Native format 65 and JSON/assets extraction version 63 protect the room-aware
version-2 `wall_split` intent within the existing exclusive command envelope 12.
The earlier six fields remain: `version`, `wall_id`, `second_wall_id`, `fraction`,
`seam_constraint_id` and `measured_owners`. Version 2 adds exactly
`physical_room_owners`, whose records have `boundary_id`, `new_segment_ids` and
`new_vertex_ids`. Completion authority follows from this dialect, not a generic
entity payload. Version-1 retained commands replay their original semantics.

Preparation partitions the physical source in a detached map, proves current
room correspondence, then freezes only the required fresh child identities.
Replay requires the complete original-current owner list, including unchanged
remote rooms that share the captured physical inventory. Ordered analytical
supports, source intervals and the full region with its holes prove each room's
continuation. Existing owner facts, classifications and deductions survive.
There is no new room-owner assignment or inferred classification in this edit.
Stale, inactive, future and unrelated rooms remain unchanged.

The retained `physical_room_wall_split` operation has exactly eight value fields:
`version: 1`, `wall_id`, `second_wall_id`, `fraction`, `source_descriptor`,
`descriptor`, `insertions` and `segments`. Each sequential insertion has exactly
`segment_id`, `new_vertex_id`, `new_segment_id` and `fraction`; final segments use
the six-field identified-geometry vocabulary. Complete descriptors chain across
room wall splits and merges, reconcile with the final room marker and supply the
digest for a following explicit repair. Full-span dimensions continue across
their new ordered segment chains; angle dimensions retain their original corner.
Supported corner relations retain their original point coordinates and raw
binding metadata; directed arc-length constraints expand over the child chain.
Version-5 tangent contact/other pairs bind the incident child at the original
contact, preserving its exact corner and directed analytical support. Both
endpoints refer to that actual child; a contact tangent is not a whole-span
length lock. Known binding metadata survives and opaque adjacent references
retain their conservative admission.
Physical wall-axis dimensions remain attached to the surviving first wall and
measure that piece, rather than acquiring an implicit whole-span target.
Automatic axis callouts follow that piece; manual text positions and presentation
remain unchanged.

Every frozen child must be fresh across entity, boundary, measured-line,
qualified construction/derivation and validated historical wall-archive
identities in all retained revisions. The bounded provenance visitor enumerates
typed identity fields, including retired original children, without assigning
authority to opaque metadata or local feature/alternative tokens.
Exact undo and redo can restore previous states. Version-2 splits cannot combine
with DISTO attachment or another authority lane. The nested intent and the
entity-only operation independently own the format-65 floor, even after undo,
deletion or abandonment of a branch. Unchanged earlier forms keep their floors.

## Source-reconstructed physical wall merges (v64)

Native format 64 and JSON/assets extraction version 62 protect exclusive command
envelope 20, whose `wall_merge` is the strict object
`{"version":1,"first_wall_id":"…","second_wall_id":"…"}`. The first wall
survives; the second is retired. Replay reconstructs the combined directed
straight span or same-circle arc from captured source walls. Ordinary entity
payloads cannot supply this authority. The independent DISTO envelope 19 keeps
its existing inner dialects; a merge cannot borrow an observation lane.

The survivor retains `extensions.wall_merge_archive` with exactly `version: 1`
and `sources`, an ordered pair of full original wall records. Each record has
exactly `id`, `type`, `properties`, `required` and `extensions`. These identities
describe historical input, not live hosts. The archive remains unchanged during
ordinary edits, splits and copies; a later merge archives its complete originals.
Archive size and nesting are bounded. Future archive versions preserve their
bytes while withholding edit authority.

Hosted opening stations move into the surviving wall's directed frame. Current
exterior owners derive their replacement outlines from the merged physical
source, retain surviving edge and corner identities, and append a strict
`wall_merge` geometry-derivation operation. Its value has `version: 1`, the
retired `vertex_id`, exact replacement `segments`, ordered `wall_source_ids` and
the retired physical `removed_wall_id`.
The derivation proves the analytical seam removal; it cannot invent measurement
receipts. Unsupported or conflicting retained references cannot disappear
silently. Undo and redo retain the complete before/after state, and retired
identities remain reserved. Commands, archives and derivation markers own the
reader floor in every retained revision, including undone or deleted geometry.

Initially current authored clear rooms continue through the same physical merge.
Their source inventories include every wall in the captured plane, so an
unchanged remote room can also require a descriptor update. Source intervals,
the complete analytical region and its holes, and surviving child correspondence
must all agree. Stale, inactive and future rooms remain unchanged. No centroid,
bounding box or scalar area match can establish room identity.

The retained `physical_room_wall_merge` geometry-derivation value has exactly
seven fields: `version: 1`, `first_wall_id`, `second_wall_id`, `source_descriptor`,
`descriptor`, `seam_vertex_ids` and `segments`. Both descriptors are complete
version-1 physical-room envelopes. Replay proves the directed inventory change,
unchanged region and any removed seams. Consecutive operations chain complete
descriptors, and the last reviewed descriptor must match the final room marker.
A subsequent room repair must name that preceding descriptor's digest. This
operation also requires native format 64 / extraction version 62, independently
of a surviving physical wall archive or command.

Every admitted merge archive reserves its retired second wall identity, including
nested prior merges and abandoned branches. Ordinary object insertion cannot
reuse it; exact history navigation can restore the original retained state.

## Per-flight stair dimensions (v63)

Native format 63 and JSON/assets extraction version 61 protect canonical
`stair` properties with `version: 3` and `form: multi_flight_stair`. The ordered
`flights` records retain their stable `id` and `riser_count`, and may add numeric
`going_m` and `width_m` overrides. An omitted dimension inherits the stair's
top-level default. Each resolved dimension must be finite, positive and bounded.
Versions 1 and 2 cannot carry operative per-flight overrides.

The authoritative layout derives each flight's run, footprint and treads from
its resolved dimensions. Connecting landings use their actual incoming and
outgoing widths; a quarter-turn landing must fit its outgoing flight. Railings
derive from that same layout. Plan-axis resize leaves elevations, total rise,
risers, level connections and stable identities intact, and can materialize
overrides for perpendicular flights. Uniform physical scaling also scales
explicit overrides. Unknown flight metadata remains attached to its stable ID.

The encoder retains the earlier v1/v2 form when no override is authored. Legacy
shared-dimension bounds still apply to that inherited form. Every retained
revision is inspected for the v3 reader floor, including undone, deleted and
abandoned stairs. Clearing an override cannot relabel its earlier history below
format 63. Quantity-entry expressions for `/flights/N/going_m` and
`/flights/N/width_m` follow stable child IDs when ordered rows move; changed or
cleared dimensions invalidate only their affected receipts.

## Planar wall tops and curved slopes (v62)

Native format 62 and JSON/assets extraction version 60 protect explicit wall-top
planes and curved walls with nonzero top rise. Native format 62 also protects
retained straight-wall geometry proof 5. The native reader-floor scan includes
every retained revision, including undone edits and deleted walls. Existing
format 61 rules remain the historical contract described below.

A wall top is one plane in model XY. Its elevation at point `XY`, relative to
`properties.elevation_m`, is `height_m + gradient dot (XY - baseline.start)`.
The optional `properties.top_plane` is the strict object
`{"version": 1, "gradient_m_per_m": [gx, gy]}`: it has exactly those two
keys, an integer version 1, and a finite two-number gradient. The gradient is
dimensionless. If `top_plane` is absent, signed `slope_rise_m` (or the legacy
`slope_rise`) derives a gradient along the baseline's start-to-end chord; a
curved wall's rise is therefore measured across its chord, not its arc length.
If both the plane and scalar rise are present, the rise must agree with the
plane's projection across that chord.

Curved walls retain their analytical arc strip and intersect its genuine prism
with the half-space below the shared plane. Hosted openings must fit below the
minimum top over the opening span and both wall faces. Splitting a wall retains
the same plane on both pieces, rebases each piece's start height, and recomputes
its scalar rise across that piece's chord. An endpoint edit retains the gradient
in world XY, keeps `height_m` at the new `baseline.start`, and recomputes the
scalar end rise for the changed chord; unlike splitting, it does not preserve
the original absolute plane. A full rigid transform preserves top height at the
mapped start and applies its rotation or reflection to the gradient; translation
does not change the gradient. Uniform physical scaling preserves the
dimensionless grade ratio. Plan-axis width/depth scaling instead applies the
inverse-transpose XY scale to the gradient so corresponding top heights remain;
it does not scale wall height or rise vertically.

## Annotations and linked dimensions in every saved view (v61)

Native format 61 and JSON/assets extraction version 59 protect sheet/view model
version 8. Plans and elevations can retain the same text, detail-line and
dimension annotations as sections. The writer selects model v8 when any
non-section view has an overlay; section-only content keeps its existing
conditional model v6/v7 encoding. Model versions 1 through 7 continue to reject
plan/elevation overlays. Public `SectionOverlay` and `SectionDimensionBinding`
type names and persisted keys remain unchanged for compatibility.

A bound dimension stores its object identity, horizontal/vertical axis in the
owning view, and line offset. Its value and witness points derive from the
complete source geometry and level placement in that view's frame. Model crop,
far depth, hidden objects and displayed detail do not shorten the measured
extent. The offset changes line placement, never the measured value. Native
solids support plans, elevations and sections. An independent room without
explicit volume measurements supports horizontal plans through its validated
line/arc footprint; no height, elevation or 3D volume is inferred. Other frames
require valid physical geometry. Missing or unsupported bound sources remain
unresolved and never fall back to saved detached endpoints.

Canvas geometry, dimension values and sheet output share the same projection
and source resolution. Candidate vertex/movement edits resolve their displayed
dimensions from the proposed source; an unresolved candidate clears an obsolete
captured line/value. Detached annotations retain their view-plane coordinates.
Section cut-plane displacement is applied once when resolving the view frame.

The reader floor includes every retained revision, including undone annotation
creation, deleted view graphs and abandoned history branches. Such content
cannot be relabelled below native v61 or extraction v59 merely because the
current head no longer contains it. Earlier projects retain their existing
reader floors. Recovery archives retain their independent recovery contract.

## Model-plan furniture and equipment symbols (v60)

Native format 60 and JSON/assets extraction version 58 protect annotation state
version 10. A symbol can opt into physical model XY coordinates with the Boolean
`model_plan: true`. Its placement, rotation, width/depth scales, local-axis flips,
pinned definition, SVG and palette remain in the existing symbol record. The
outer annotation entity envelope and its owning layer contract do not change.

The marker is optional in state v10: absent or false retains the legacy view
overlay convention. Earlier annotation states reject the marker, including
explicit false, and non-Boolean values are invalid. V10 retains the extended
styles admitted by v9. The writer selects v10 when a model-plan symbol exists;
states containing only earlier semantics keep their conditional older version.
Loading a legacy symbol never reinterprets its saved coordinates as model XY.

Model-plan symbols use the same horizontal plan projection as authored geometry.
The complete physical footprint and SVG orientation follow the plan axes,
including reflected views. A Site annotation instead resolves its saved child's
layer and declared presentation frame once; it does not add a second conventional
plan transform. View-overlay symbols retain their existing presentation path.
Joint connected translation applies the model offset to model-plan symbols and
labels, and the separate presentation offset to legacy overlays. Translation
cannot change the coordinate mode or any unrelated symbol property.

The native reader floor includes every retained revision, including undone
creation, deleted symbols and abandoned branches. Such an archive cannot be
relabelled below v60 even when its current head has no model-plan symbol. Older
projects without these semantics retain their existing format floors and remain
readable. Extraction uses version 58 for this floor in both ordinary and compact
asset output.

## Atomic geometry and device observations (v59)

Native format 59 and JSON/assets extraction version 57 retain command-envelope
19: a typed geometry edit followed by one `disto_measurement` attachment in the
same history event. The outer envelope contains `expected_revision`, `message`,
`disto_measurement_completion: true`, `disto_measurement` and `proof`. The
attachment has exactly `owner_id`, `record` and Boolean `replace_existing`;
`proof` is the original typed command envelope without an attachment. Nested
attachments are refused. Existing geometry dialects keep their admission rules.

Replay first reconstructs the complete original geometry, then attaches the
record to the same existing owner. The record's field must match the owner type
and its value, converted from the declared unit, must agree with the completed
dimension. Wall lengths use the physical straight or curved axis with only
floating-point representation allowance; scalar fields use the exact converted
value. Display rounding and solver tolerance do not qualify a different reading.
The attachment cannot alter geometry, properties, identities, receipts, assets
or unrelated extensions. Replacing an existing record requires
`replace_existing: true` and a valid prior record for that field.

The observation remains in the existing version-one `disto_measurements`
extension, indexed by its target field. It records the reading, declared unit,
device identity, timestamp, transport and provenance. A later ordinary geometry
edit may leave this historical observation unchanged; an old reading is not a
claim about the current dimension. Keyboard provenance retains the received text
and explicit unit/decimal declarations without claiming device discovery or
physical qualification.

The reader floor includes every retained command, including an undone edit or a
deleted owner. Relabeling an archive below v59 cannot make envelope 19 readable
by an older reader. Ordinary entity commands can retain measurement metadata
through their existing admission path and do not acquire typed geometry
authority from that metadata. Older projects keep their existing format floors.

## Site presentation frames, terrain datum bindings and framed annotations (v58)

Native format 58 and JSON/assets extraction version 56 protect explicit
site-coordinate contracts: `property.site_frame`, `building.site_placement`,
`terrain_surface.terrain_elevation_binding`, and `presentation_frame` on
non-container entities. Annotation entity version 3 requires its own strict
`presentation_frame`; each label or symbol still uses its own saved layer to
determine the source context.

Site and building transforms use metres, XY yaw and Z translation. They do not
scale, shear or rewrite authored geometry, measurements, or vertical-level
facts. Building geometry enters the property-site and relative building
transform only when `site_placement` is explicitly present. Terrain ignores
building placement: relative elevations bind to the site origin, while
absolute elevations require the property's matching vertical datum. Independent
assembly geometry remains in its existing world frame unless its
`presentation_frame` explicitly enrolls it. Placement is resolved for
presentation after local hosts, joins, assembly expansion and vertical
placement; the saved source remains local and unchanged.

The reader floor scans every retained revision, including undone, deleted and
abandoned owners. Unknown or malformed future markers raise the conservative
reader floor but gain no typed authority. Container entities cannot override
their presentation frame. Older projects without these explicit markers keep
their existing world coordinates; loading or extracting them does not infer a
site placement or migrate geometry. Their recovery ledger and original source
bytes remain intact.

## Independent assemblies, roof materials and living units (v57)

Native format 57 and JSON/assets extraction version 55 protect independent
`assembly_instance` entities, `assembly_model.model` catalogs with schema
`sketch.assemblies.v4`, `roof_join` version 2 or its `material_assignment`, and
property/boundary `appraisal_reporting` version 2. Reporting markers containing
`living_units`, `living_unit_id`, or a room's `other_description` also require
this floor. The scan includes every retained revision, including undone creation
and deleted owners. Relabeling such an archive as v56 refuses even with a
recomputed logical digest; refusal preserves its bytes. Existing v56/exchange54
reporting and earlier format contracts continue to apply.

Independent instances use the entity envelope `version: 1`,
`form: independent_assembly_instance`, `assembly_catalog_id` and `instance`.
The nested instance uses `sketch.assembly-instance.v1`, has the same identity
as its entity, and requires a world `root_transform`. Legacy host placement and
top-level placement/type aliases are forbidden. Catalog references, type IDs,
material slots and nested part paths are validated against the actual catalog.
V4 catalogs retain compact extrusion profiles, nested parts, independent root
transforms and structured nested overrides; geometry remains derived from
those authored values. Unrelated entity metadata remains preserved.

Authored profile and part order remains stable. Expanded geometry retains its
root identity and typed part path, type and profile provenance. Each expansion
is limited to 4,096 nodes and 262,144 profile segments; the same limits span all
placed roots in a document. Validation of unused definitions has a separate
aggregate limit of 16,384 nodes and 1,048,576 profile segments. A shared nested
definition cannot evade those limits by expanding through several branches.
Rejected graph changes preserve the previous document and retained history.

Roof joins retain their ordered `roof_ids` and fused style. Version 2 adds
`material_assignment` with `version: 1`, `catalog_id` and `material_id`; both
the assembly catalog and its material must exist. This assignment belongs to
the join and does not replace the retained member roof geometry.

Property reporting v2 adds one to 64 `living_units` with unique bounded
`unit_id` and appraiser `identifier`, declared `role`, and optional level
declarations. Boundary reporting v2 can reference a declared `living_unit_id`
and adds the expanded room-use vocabulary; `other` requires a nonblank
`other_description`. The existing source observation digest still fences
reporting facts. These declarations are authored facts, not inferred from
decorative geometry or owner names. Typed codecs reject unsupported fields
and malformed declarations; a reader-floor marker does not grant authority.

## Appraisal reporting and conditional declarations (v56)

Native format 56 and JSON/assets extraction version 54 protect either an
`appraisal_reporting` marker on a property or measurement boundary, or
`appraisal_policy.ansi.limitation_declarations` on a property. The reader floor
inspects all retained history, including undone creation, deletion and abandoned
branches. Future or malformed markers still require the floor; they do not
acquire executable authority. An identically named vendor field on another
entity type remains ordinary opaque data. Lower format labels are refused even
when their logical digest has been recomputed.

Property `appraisal_reporting` version 1 contains `contract` (`legacy_uad_2_6`
or `uad_3_6`) and an explicit Boolean `room_inventory_complete`. Boundary
reporting contains `version: 1`, `source_geometry_sha256`, optional Boolean
`contained_within_primary`, and `rooms`. Each room has a unique, bounded
`room_id`, a declared `use` (`bedroom`, `bathroom_full`, `bathroom_half` or
`other`), and optional Boolean `legacy_total_room`. Unknown fields, versions,
uses and malformed values refuse typed interpretation. Room identifiers are
membership identities; names and decorative fixture symbols are not room facts.

The persisted digest field binds current geometry, deductions, relevant area
and inherited floor observations, property measurement policy and ownership.
Reporting's own fields, names and appearance are excluded. Changed observations
require explicit reconfirmation; a new transaction fingerprint does not renew
old declarations. Applying reporting uses one undoable command fenced by the
entire captured snapshot, including retained history, and the same semantic
design-phase scope used in the editor. Presentation hiding does not remove an
area from appraisal calculations.

UAD 3.6 primary room counts use declared original room types across grades and
finish categories. ADUs and noncontinuous area retain separate summaries.
Combined ADU summaries are not per-unit form fields. Legacy room mappings that
remain unresolved withhold primary counts while preserving supported separate
summaries. Report projection never changes canonical independent ADU areas.
Absent reporting configuration leaves older projects in measurement-summary
mode. This schema does not certify ANSI, lender forms or vendor interchange.

Conditional ANSI declarations are a bounded array of `{kind, statement}` records.
Kinds are `interior_not_inspected`, `based_on_plans`, and
`direct_measurement_not_possible`; each requires an explicit nonblank statement
when its measurement condition applies. Generic `limitations_statement` remains
supplemental notes. Duplicate, unknown and inapplicable kinds are rejected for
qualification. Saved incomplete facts remain editable but cannot produce
qualified totals. Presence checks do not verify prescribed publisher wording.

## Physical wall axis dimensions (v55)

Canonical dimensions with `dimension_version: 4` and
`dimension_kind: wall_axis_length` target the actual physical wall through
`target.entity_id`. They carry no segment, vertex or segment-chain target.
Resolution reads the current physical axis and its analytical length; a stored
observation is not measurement authority. Only this version-4 kind is supported:
version-4 prior kinds and future versions or kinds remain opaque and retain the
complete source entity. Malformed known typed data is rejected.

Native format 55 and JSON/assets extraction version 53 are required when a
supported wall-axis dimension occurs in any retained revision, including undone
creation or deletion. Existing dimension versions 1–3 keep their contracts.

## Owned stair landing railings (v54)

Canonical `railing` properties with `version: 3` and
`form: stair_landing_railing` persist height, thickness and post spacing plus a
`host`. The host names `stair_id`, `role` (`connecting` or `top`),
`incoming_flight_id`, `edge_index` (0–3), and ordered start/end station fractions.
A connecting host also names `landing_id` and `outgoing_flight_id`; a top host
cannot carry those fields. Hosted geometry derives from the stair landing;
independent base position, orientation and length are rejected, as are flight
host fields on a landing rail. Unknown forms and future integer versions retain
their opaque data without acquiring canonical attachment authority.

Native format 54 and JSON/assets extraction version 52 apply across retained
history, including deleted, undone and abandoned records. The version-2 flight
railing contract remains unchanged; extra version-2 host keys confer no landing
authority. These schema additions introduce no separate replay-version field.

## Assistance previews and extraction limits

Assistance proposals use `schema_version: 1`, `status: unverified`, and
`requires_explicit_acceptance: true`. Their source observations, resource
provenance and command preview are not executable document commands. Acceptance
creates a request for normal command validation, permission checks and an Undo
transaction; it does not verify measurements. The decoder rejects unsupported
schemas, unknown kinds and unexpected envelope fields rather than granting them
opaque executable meaning. This preview schema has no replay-version field and
does not define document-history authority.

JSON/assets extraction preserves retained revisions and, when an archive is
supplied, its recovery records. Its exchange version follows the highest native
reader floor required by that history. Extraction remains separate from a JSON
project importer; an assistance preview or extracted record does not itself
establish a replayable accepted command.

## Exact physical-source translation lineage (v53)

A measured exterior translated with its physical walls can retain
`wall_measurement_source.version: 2`. Its frozen kernel, original wall
baselines/thickness/context, original analytical outline, and flat ordered
translation list reproduce exact coordinates without summing offsets or
subtracting a translation to guess original values. Captured physics must
independently derive the original outline. Each translation is replayed over
both the physical baselines and the outline; live source-currentness also
requires exact current wall records. Intrinsic lineage remains valid when a
physical source has subsequently changed or disappeared, so stale values are
withheld without discarding valid history.

Typed boundary edit v8 retains the exact translation offset and includes
genesis captures only when upgrading a v1 source. Further translations append
to the existing validated lineage. The document reconstructs the complete
submitted edit from its actual source and physical result. Ordinary shape,
thickness, or source replacement derives a fresh v1 source. Limits are 2,048
captured walls, 4,096 finite nonzero translations and a bounded encoded proof.
The combined replay budget is 8,388,608 wall-pair checks:
`W * (W - 1) / 2 * (T + 2)`, where `W` is the captured wall count and `T`
the retained translation count. The independent limits cannot be multiplied
into an unbounded replay workload. Exhaustion refuses the edit atomically
rather than changing coordinates.

Native format 53 and extraction 51 cover this authority in every retained
revision, including undone/deleted history and recovery. Lower format labels
are rejected even when their logical digest has been recomputed.

## Typed stair topology and owned flight railings (v51/v52)

Canonical `stair` properties with `version: 2` and `form: multi_flight_stair`
require native format 51 and extraction 49. Flights and connecting landings have
stable child identities owned by the stair. A retained child ID cannot be reused
for another owner or role, or become an entity ID; retirement and abandoned
Redo history preserve that lifetime. Ordinary Undo/Redo restores the original
identities. New topology and clones allocate new identities.

Canonical `railing` properties with `version: 2` and
`form: stair_flight_railing` require native format 52 and extraction 50. The rail
owns a reference to its current stair and typed flight, side, and ordered
start/end station fractions. Geometry follows that flight's tread pitch line;
it has no independent base position, orientation, length, or vertical placement.
The host and rail require complete compatible property/building/floor/layer
organization and compatible phase membership. Attachment and identity checks
apply to restored document history as well as current edits.

These reader floors inspect every retained revision, including deleted stairs,
removed rails, undone commands and abandoned branches. Both native markers and
the logical digest carry the selected floor; recomputing a digest after lowering
markers does not permit opening that history with an older reader. Version-1
straight stairs and independent railings, and unrelated opaque forms, retain
their previous reader floors. No SQLite columns change.

## Source-qualified physical-room dimensions (v50)

Existing dimension versions 1–3 can target an identified physical-wall-derived
room. Structural admission retains stable edge, corner and chain references;
it does not certify a stored room outline as current. Numeric resolution requires
the complete current entity map or snapshot. It rederives clear spaces from the
physical walls, checks exact source lineage, outer geometry, holes, drawing
context, active phase and effective plane, and subtracts holes from clear area.
A changed source withholds numeric values until explicit room review.

Any such dimension anywhere in retained history requires native format 50 and
extraction 48, including undone or deleted dimensions. This changes neither
SQLite tables nor the dimension schema. Entity-only numeric resolution remains
unsupported for physical rooms; anonymous legacy or unqualified room geometry
cannot substitute for the physical source.

## Atomic reviewed physical-room dispositions (v49)

Command envelope 18 carries a strict version-one semantic review intent and an
explicit completion marker. It contains complete retained-room and fresh-space
decisions for one captured physical context and plane, explicit identities and
metadata for created rooms, interior witnesses, retained descriptor digests,
complete Keep/Remove choices for affected supported references, explicit child
mapping for kept edge/corner references, fresh automatic dimension IDs, and
acknowledged room-relationship removals. Complete reviewed retained owners can
exchange destinations; ordinary single-room repair still refuses an occupied
destination. Regenerated automatic dimensions preserve their template's style,
context and opaque metadata while taking fresh identities and analytical targets.
Heterogeneous kept automatic templates refuse rather than silently choosing one
style. Unknown incoming references to removed dimensions or constraints also
refuse. The new batch policy does not reinterpret older ordinary-command proofs.

The authoritative replay rederives physical clear spaces and all resulting
entities from the preceding source. Ordinary geometry, entity or asset lanes
cannot be combined with this authority. Live application checks the full captured
snapshot. Historical replay validates the exact preceding entity map, a version-2
authoring-source digest, and the complete captured snapshot reconstructed from
the retained prefix and captured saved revision. Both include every typed proof,
assets, names and navigation. The frozen version-1 authoring digest retains its
original representation and does not authorize this review. New identities cannot
reuse retained history. Opaque references in JSON keys or values refuse when
retirement or child replacement would detach them. Cancel
publishes nothing; the complete accepted batch is one history event. Native
format 49 and extraction 47 retain this authority after Undo/Redo and reopen.

## Persistent analytical tangent junctions (v48)

Constraint entity version 5 is exclusive to the `tangent` relation. Its four
bindings are the first contact, first opposite endpoint, second contact, and
second opposite endpoint. Each pair identifies one full native segment with
opposite endpoint roles. At least one segment is a circular arc. Signed sweeps
and endpoint derivatives come from authoritative owner geometry.

A smooth junction requires coincident contacts and opposite outward tangent
directions within the defined linear and angular tolerances. Chord parallelism
does not establish tangency. The solver enforces this relation during subsequent
edits; existing fixed anchors remain hard constraints. Earlier entity versions
cannot interpret tangent, and v5 cannot reinterpret earlier relations. Native
format 48 and extraction 46 cover the head and retained history.

## Joint connected translation (v47)

Command envelope 17 carries a version-one joint translation intent and one
ordinary geometry proof. It records the selected rigid boundary and measured
stroke IDs, selected partial physical-wall IDs, finite nonzero offset, and
connected-movement choice, selected dimension IDs, and nullable finite
`presentation_offset`. The analytical offset applies to model geometry and
model-space labels; a separate presentation offset can move selected overlay
symbols, labels and references through a projected view. All selected points participate in one hard-connected
solve. Persisted fixed anchors and relations retain their original authority.

Replay reconstructs that solve from the preceding source, rederives measured
owners and callouts, and requires the submitted proof to produce the exact same
entities. Existing annotation/reference supplements may change placement only;
their types, child identities, size, content, styling, context and opaque data
remain unchanged. Geometry supplements cannot borrow presentation authority.
Independent disjoint composition remains envelope 16. A missing intent,
false completion marker, nested authority, changed relation, or forged result is
refused. Native format 47 and extraction 45 apply to retained history, including
undone edits and stripped proofs; earlier readers cannot ignore this intent.

## Scoped annotations and explicit presentation (v46)

Annotation state version 9 adds optional `style.fill_opacity` in `[0,1]`,
`style.line_pattern` (`solid`, `dash`, `dot`, `dashdot`), and `cross`,
`horizontal`, and `dots` fill patterns. Absent opacity preserves earlier screen
and output alpha defaults. Explicit opacity, including 0 and 1, is retained.
The optional `use_model_text_height` callout flag uses the style's metre height
instead of the default paper height, without changing earlier callouts. It
cannot coexist with explicit paper text height. Earlier states reject new keys.

Outer annotation entity version 2 retains a complete property/building/floor/layer
context, with an optional level. Outer version 1 remains unchanged. Reusable
text library version 3 carries these styles; earlier library formats refuse
unsupported fields. Native format 46 and extraction 44 apply to retained history,
including removed or undone annotations.

Pinc imports retain the exact source file once as an `application/x-pincsketch`
asset. Source pointers and page display mappings are provenance in extensions;
native measured-line receipts and source-derived areas remain the geometry
authority. Decoded underlays keep original encoded assets and PNG previews.
Source categories never establish appraisal eligibility or physical wall depth.

## Independent live area callouts and text alignment (v45)

Annotation state version 8 adds `style.text_alignment` (`left`, `center`, or
`right`) and presentation override roles `area_name` and `area_calculation`.
Both reference the existing area owner through `target_id`. Each role retains
its own text style, visibility, model-space anchor offset, optional paper text
height and rotation. It cannot supply geometry outline or hatch-spacing fields.
The area name and calculated string are derived at render time; these overrides
contain presentation only. A hidden callout does not hide its analytical area.

The encoder uses version 8 when a role or noncenter alignment is authored.
Centered legacy annotations retain their prior schema and appearance. Earlier
annotation versions reject the new alignment field and role names; version-8
siblings without an alignment field default to centered. The outer annotation
entity schema remains version 1. Reusable text-library version 2 retains
alignment; older centered library records keep their existing wire format.

Native format 45 and extraction 43 are required wherever version-8 annotations
appear in retained history, including Undo or deleted presentation owners.
Both native format markers and the portable-package validator recognize this
reader floor. Older readers must refuse these files before losing callout
semantics. No SQLite table change is required.

## Reviewed same-ID physical room repair (v44)

Boundary geometry edit dialect 7 adds `physical_wall_room_repair`: a selected
current wall ID, finite strictly interior witness, exact reviewed detector
lineage and SHA-256 of the retained physical-room descriptor. It is an exclusive
fresh-topology boundary redefinition: no unrelated source lane, authoring data or
classification override is allowed. Fresh child identities and explicit existing
reference mapping/removal contracts remain required.

Document apply and retained-history restore independently detect the destination
from the preceding entity map. They verify context, phase, old descriptor digest,
exact outer geometry, current lineage, strict interior membership and destination
ownership. Holes are regenerated from that source; they are not supplied by a
caller. Only the verified same-ID edit can replace a retained room's marker and
outline. Name, classification and unrelated metadata remain intact; previous
evidence remains in history. Undo and Redo navigate that validated history.

This is explicit reviewed reassignment, not automatic cell correspondence. A
split assigns only the chosen space; other pieces remain unclassified. A merge
does not acquire other rooms' metadata, and other affected owners remain stale.
Automatic correspondence and atomic multi-room dispositions remain open.
Entity-only dimensions on physical rooms remain refused until their
snapshot-aware resolution is implemented.

Retained dialect-7 repair intent requires native format 44 / extraction 42,
including a deleted owner, retained history or an entity-only geometry-derivation
carrier. Unsupported older readers must refuse before accepting the history.
An entity-only derivation carrier preserves the higher reader floor; it does not
by itself verify live source authority. Current room queries still rederive it.

## Source-bound clear rooms (v43)

An identified `room_boundary` can carry `extensions.physical_wall_room`.
Its four fields are `version` (1), `selected_wall_id`, `source_lineage` and
`holes`. Version 1 requires a detector-produced version-one lineage, a valid selected wall ID, and
bounded analytical holes encoded as segment arrays with `[x,y]` start/end points
and signed `sweep_radians`. Exact adjacent endpoints are required. The outer
identified boundary remains the owner's authoritative retained topology.

The canonical query rebuilds the complete source context/elevation/semantic phase
and checks exact outer, holes and lineage before exposing clear room area.
Unknown positive descriptor versions are retained read-only. Stale evidence has
no current quantity. Generic edits cannot strip or replace the descriptor or
independently move the outer. Classification is separate from appraisal facts;
these room owners do not enter exterior GLA.

The marker in any retained revision requires native format 43 / extraction 41,
including after deletion. No new command-envelope version is needed for this
creation/stale checkpoint. Historical centerline rooms are not automatically
converted. The reviewed same-ID repair dialect is documented above.

IFC source carriers exceeding the legacy small-text limit use private
`Pset_VertexExchange_v2`: one owner, a count/byte/SHA-256 manifest and ordered
text chunks. Reconstruction is bounded and checksum-verified; imported carriers
retain opaque native evidence through repeated exchange without activating a
physical room. Supported flat footprint loops and source retention are distinct
from IFC semantic room or full architectural interoperability certification.

## Optional linked floor tracing

A floor may retain `properties.tracing_reference` as a presentation preference:

```json
{
  "version": 1,
  "source_floor_id": "source-floor-id",
  "visible": true,
  "opacity": 0.25,
  "offset_m": { "x": 0.0, "y": 0.0 }
}
```

Version 1 requires exactly these fields, a nonempty source ID, boolean visibility,
opacity from 0.05 through 0.75 and finite metre offsets within +/-1,000,000. The
source and destination must be distinct floors with resolved organization in the
same building and property. Unknown versions or malformed fields are preserved
as metadata but refused by the tracing projector, with a repair diagnostic.
Clearing the reference removes the preference in one undoable revision.

The source is resolved live from the current document; geometry, annotations and
assets are not copied into the destination floor. The XY offset is applied only
to screen rendering. While the destination is active in the 2D workspace, its
ordinary canvas focuses on that floor independently of reference visibility.
The reference does not participate in quantities, selection, snapping, extents,
sheet output or sketch export. The link persists through native save/reopen and
Undo/Redo. Existing v42 generic entity properties already preserve this optional
presentation metadata; it introduces no new geometry or command dialect.
Earlier clients may preserve the preference without offering its tracing UI.

## Mixed rigid group and connected geometry completion (v42)

Constraint-command envelope 16 retains an optional `rigid_group_transform`
typed transform group and the `rigid_group_completion` discriminator. This
authority composes a rigid group move with connected geometry consequences in
one revision. The child transform keeps its existing typed representation;
earlier command envelopes 1 through 15 keep their historical rules.

Retained mixed completion intent requires native format 42 / extraction 40,
including undone commands and deleted states. Either the completion marker or
the optional transform child sets this reader floor. Clearing the child cannot
downgrade a retained completion marker. Readers reject a downgraded native or
extraction format instead of dropping the proof. Compact supplemental asset
references remain supported in envelope 16 through their existing discriminator.

## Connected geometry with explicit dimension placement (v41)

Constraint-command envelope 15 retains `dimension_placement_moves`, a list of
dimension IDs and model-space offsets, and its explicit completion discriminator.
This narrow authority composes a connected geometry edit with selected callout
placement in one revision. It does not permit raw dimension supplements, source
replacement or removal of geometric constraints.

Replay resolves each dimension against the original and reconstructed geometry.
After exterior redraw and measured-source consequences, it uses the original
text position plus the retained offset, sets manual placement and clears the
automatic-placement version. Stable analytical target identities, presentation
style and opaque metadata remain intact. Invalid or retired targets, conflicting
raw edits, duplicate intents and nonfinite/overflowed coordinates refuse the
complete command. Unselected automatic dimensions retain normal source reflow.
Ordinary selected labels or reference images may accompany this typed placement
lane without implying an exterior-source redraw. Explicit exterior and measured
completion flags still require their corresponding reconstruction proofs. The
legacy supplemental inference for envelopes 1 through 14 remains unchanged.

Retained envelope-15 history requires native 41 / extraction 39, including Undo
and deleted states. Emptying the placement vector cannot downgrade the envelope;
an empty completion cannot authorize a live edit. Entity-only materialization
without retained placement intent keeps its existing reader requirements. Earlier
command dialects and the isolated geometry importer protocol remain unchanged.

## Physical-source measured curve reconstruction (v40)

Command envelope 14 retains an `exterior_segment_arc` intent containing the
stable measured boundary and segment IDs, the exact chord ConstructionReceipt,
and the related-object movement choice. Angle, height and arc-length inputs
retain their expressions and exact values. Both measured chord endpoints and
all unselected measured geometry remain fixed within analytical roundoff.
Physical wall baselines are reconstructed from that requested exterior using
their original unequal thicknesses. Their derived curve inputs describe the
physical baselines, independently of the measured construction receipt.

Replay reconstructs the physical perimeter independently and checks the entire
final measured outline after source completion. Competing raw physical,
supplemental, rigid, split, corner or resize authority is forbidden; frozen
related objects cannot carry dependent wall, boundary or measured-stroke edits.
Current measured consumers and attached dimensions derive from the resulting
walls. Ordinary topology, host, source-currentness and constraint checks remain.

Straight-to-arc conversion uses curve_input_derivation version 3 with null
source_input and the exact original straight source_baseline. Its first operation
records the derived physical arc input. Later ordinary, construction and rigid
operations preserve that line origin and archive prefix. Earlier versions keep
their historical curve-origin rules. Retained envelope-14 commands and v3 line
origins require native 40 / extraction 38, including undone and deleted history.
Entity-only snapshots with that origin require the same reader floor; histories
without the new proof or provenance retain their previous minimum versions.

## Physical-source measured edge resizing (v39)

Constraint-command envelope 13 retains an `exterior_segment_resize` intent:
the current measured boundary and stable segment IDs, exact entered length
receipt, fixed start/end endpoint, local boundary-chain movement and movement
of other connected objects. The two movement choices are independent.
Reconstruction of the measured boundary's physical perimeter is mandatory.

Replay reconstructs the requested analytical outline, inverts its wall offsets,
validates physical contacts and constraints, and regenerates current measured
consumers. The final selected edge must retain its requested analytical length,
anchor and signed sweep. Physical wall arc sweeps can change when their offset
joins are trimmed. Derived boundary coordinates remain the actual forward result;
the requested outline cannot substitute for source-derived geometry.

The new intent cannot borrow ordinary physical, asset, split, corner or rigid
mutation authority. Dependent measured-stroke completion retains envelope 13.
All retained commands with this intent require native 39 / extraction 37,
including undone and deleted histories. Entity-only materialized imports with
no retained resize intent keep their existing entity/archive reader floors;
they do not claim to preserve the absent entered-length receipt. Earlier command
dialects retain their existing representation and replay semantics.

## Whole-wall relationship membership (v38)

Room relationship schema two retains ordered physical members for one logical
architectural wall. Splitting a member rewrites the owned list without changing
relation endpoints or meanings. Native format 38 and extraction version 36 are
required when schema-two or future relationship models appear in any retained
state, including Undo, deleted records and entity-only extraction. Ordinary
schema-one records and vendor properties on unrelated types keep prior floors.
Downgraded native/extraction markers reject instead of dropping membership.

Known schemas validate all member roles, current physical wall geometry and the
combined graph across records. Positive future models remain opaque and make
the project read-only. Retained known wall-split proofs are still reconstructed:
only future relationship records are removed from the detached replay input,
then restored exactly before full reconstructed-state comparison. Authored
splits retain strict reference protection; this restoration exception cannot
authorize a new split against unknown references.

## Wall point insertion and whole-span dimensions (v37)

Wall split command authority, physical arc-chain relations and full-span
dimensions require native format 37 and extraction version 35. The typed split
reconstructs both baselines, hosted opening stations, supported constraint and
measurement consumers, and retained archive evidence. Unknown references and
openings crossing the seam refuse atomically. Reconstructed full-state equality
precedes detached history normalization.

## Measured stroke dimensions (v36)

Saved dimensions targeting identified measured strokes require native format 36
and extraction version 34 across retained history. Stable target identities and
authoring provenance remain part of the document; display rounding does not
replace analytical geometry.

## Persistent measured-stroke constraints (v35)

Measured linework schema/replay v5 adds ordered `vertex_batch` operations. Each
contains `edits`, an array of typed `move_vertex` edits naming unique existing
stable vertices of the same stroke. Replay installs every target before final
geometry validation. Original construction receipts, identities and signed
sweeps remain authoritative. Existing edit and transform operations keep their
semantics; entered resize quantities and reflected arc sweeps are not replaced
by solver coordinates. Empty or all-no-op public batches preserve the original
dialect; meaningless persisted batches are rejected.

Constraint command envelope 11 retains independent Boolean measured, rigid-wall,
exterior and supplemental completion modes. Its `measured_stroke_edits` lane
names a stroke, optional authored edit/quantity or rigid transform, and final
vertex edits. The authored edit and transform are mutually exclusive. Geometry
and eligible measured consumers are independently reconstructed before final
validation; raw area payloads do not supply this authority. Previously stale,
ambiguous or authored consumers remain subject to explicit source review.

Native format 35 and extraction version 33 apply across all retained revisions,
including Undo and deletion. Already-satisfied relation-only stroke bindings
also require this floor, even when the stroke itself remains v1. No SQLite
columns change. Downgraded markers reject; unsupported stroke payloads remain
preserved under read-only protection rather than being interpreted as boundaries.

## Curved survey source provenance (v34)

Survey call reports use version 2 when any call is a circular arc. The typed
`measurement_boundary.extensions.survey_source` wrapper then uses version 2,
including after a later correction back to straight calls. Current and original
reports retain entered text, exact quantity receipts and their own report/input
versions. The common identified boundary model stores analytical signed sweeps;
no new geometry representation or SQLite columns are introduced.

Native format 34 and extraction version 32 protect this source contract across
all retained revisions, including Undo, deleted owners and entity-only imports.
A v1 wrapper carrying v2 or future report/input metadata cannot lower the floor.
Future or malformed typed discriminators preserve their JSON with read-only
protection; collisions on unrelated entity types remain opaque. Ordinary v1
straight-call projects retain their previous reader floor.

Document admission checks source structure and versions, rather than treating
archival calls as live geometry. Ordinary drawing edits and transformations may
intentionally differ from the recorded calls. Opening those calls or correcting
a boundary requires sealed reconstruction from entered expressions and matching
receipts. Derived report vertices and diagnostics are recalculated. Loading a
project therefore does not certify the semantic accuracy of its archival survey
receipts. Future payloads remain preserved without being consumed.

The report contains `provenance`, ordered `legs`, local `vertices`,
`closure_tolerance_m`, `diagnostics` and `input_provenance`. V2 legs add signed
`sweep_radians`; `distance_m` remains the chord distance. The v2 entered-input
record retains `legs_text`, `source_text`, `closure_tolerance_expression`,
`default_unit` (`m` or `ft`), ordered `distances` and ordered `curves`. Each
distance receipt records `leg_id`, `line_number`, `original_expression` and
`exact_metres` with integral `numerator`/`denominator`. Each curve receipt uses
`version: 1`, `leg_id`, `line_number`, `construction_kind` and
`original_expression`. `chord_angle` retains normalized `sweep_radians`;
`chord_height` retains signed `exact_metres`; `chord_arc_length` retains measured
arc `exact_metres` plus Boolean `clockwise`. Chord and arc-length receipts are
distinct. Positive sweep/height is counter-clockwise, and negative is clockwise.

The source wrapper retains `report`, optional `original_report`,
`added_closing_segment`, `adjusted_final_endpoint` and `endpoint_adjustment_m`
(`null` or an object with `east`/`north` metre offsets). The first correction
also retains `original_closure` with those closure fields. `placement` records
its current anchor and called-north orientation. Derived diagnostic totals and
vertices are never accepted as replacement input; a reconstructed report is
required before generating or correcting a boundary.

## Grouped measured regions (v33)

A `measurement_boundary` can retain the union of at least two adjacent detected
measured faces. Its `extensions.measurement_linework_sources` remains the exact
outer boundary lineage. The additional `extensions.measurement_linework_group`
record has exactly `version: 1` and `members`, an array of at least two original
face lineage arrays. Each member uses the existing outer-style representation:
an ordered array of edges, each containing source uses with `owner_id`,
`segment_id`, `parameter_start`, `parameter_end` and `reversed`.

Member evidence retains internal seams cancelled from the combined outer
outline. A missing, hidden or changed seam source can therefore invalidate the
group even when its outer outline appears unchanged. The group does not
tessellate arcs, infer classifications or automatically deduct holes. The
single-loop combination primitive rejects disjoint selections, point branches,
nested overlapping outlines and selections that leave holes or multiple loops.

Any appearance of this reserved marker on a `measurement_boundary` requires
native format 33 and extraction version 31, including malformed, future or
missing-outer payloads and Undo/Redo or deleted-owner history. Qualification
does not depend on the current boundary model version or member validity.
Recomputing a digest cannot lower the reader floor. No SQLite columns, raw asset
encoding or existing command dialects change.

Unsupported top-level group schemas preserve their complete JSON and make the
document read-only. An understood version-one group with invalid or stale
member evidence retains its bytes while source checks withhold dependent
operations. A vendor key collision on an unrelated entity type retains the
previous format floor and does not acquire group semantics. Existing projects
without grouped measured regions keep their earlier native/extraction floors.

## Finished-room ceiling rule (v32)

ANSI-oriented policy version two uses calculation profile
`vertex-ansi-z765-2021-v2`. Sloped observations retain the gross
`room_floor_area_m2` as a geometry-binding check, and require Boolean
`complete_room_observed: true`. The half-height comparison uses current net
physical room geometry after the union of its real exclusions. Its numerator
must be observed for that same complete physical room outside those exclusions.
Scalar-only derivation, zero candidate area, missing confirmation, stale
bindings and excessive high-area observations cannot qualify a contribution.

A V2 sloped room may contain exclusion boundaries, including open-to-below and
low-height regions. It cannot treat measured child partitions as voids or
independently apply the room threshold to an arbitrary ownership remainder.
Flat floor → complete sloped room → low-height exclusion nesting remains valid.

Setup offers an explicit rule selector. Existing V1 projects retain their gross
room interpretation, and opening a dialog never migrates them. Choosing V2
preserves observations but withholds unconfirmed sloped totals until Edit facts
records the complete-room confirmation. Save/Cancel and Undo/Redo use ordinary
document commands. Details, schedules and PDF identify the selected rule; the
ceiling trace exposes the unrounded denominator and numerator.

Policy V2 or complete-room confirmation anywhere in retained history requires
native format 32 and extraction version 30, including undone/deleted entities.
Older readers refuse these semantics. Recomputing a digest cannot lower that
floor. V1-only evidence retains its existing format-21/extraction-19 floor.
This is a sourced Vertex rule interpretation, not final ANSI certification.

## Manual dimension presentation in recovery

Define First dimension orientation and omission use the existing version-two
`area_dimension` presentation, including `visible` and `rotation_radians`.
Unfinished and historical sessions with this presentation use boundary recovery
checkpoint schema 3 / replay 1. The checkpoint scans the entire retained action
timeline, including Redo-only placements, and canonically replays presentation.
See [dimension presentation checkpoints](boundary-recovery.md#dimension-presentation-checkpoints).

The native container supports v1–33. Recovery-bearing files retain the existing
container floor; older readers encounter an unknown nested checkpoint and
preserve the whole ledger opaquely without granting editable state. Completed
dimension presentation and JSON/assets recovery extraction already have their
required representation. New native/extraction version numbers are unnecessary
for this presentation extension.

## Exact chord length and heading (v31)

A standalone construction receipt with `version: 2` stores `chord_input` as
exact `length` quantity and `heading` angle records. The three chord-based arc
kinds require exactly one definition: this input or the legacy `chord_end`.
The endpoint is derived analytically from the retained local start, length and
heading. Original expressions, normalized expressions and entered units remain
part of the authority. Unused legacy receipts retain their unversioned encoding.

Boundary construction schema 4 / replay 1 permits typed receipts and retains the
schema-3 frame representation. Measurement linework schema 4 / replay 4 retains
the ordered schema-3 transform/edit representation. Promotion preserves existing
operations and their order. Transforms do not rewrite original typed inputs.
Recovery checkpoint schema 2 / replay 1 permits typed receipts, including closed
chains and undone actions; legacy checkpoint schema 1 remains unchanged. Older
known dialects reject typed input, and unknown positive versions remain opaque.

Native format 31 and logical extraction version 29 are required for typed
receipts, including retained Undo/Redo history, archived boundary source proofs,
replacement construction in redraw edits, nested reconstruction commands and
original wall input. Wall receipt validation is independent of an unknown future
context dialect; a known context additionally proves the original local frame.
It is not compared with an edited current wall baseline. No SQL or raw asset-row
representation changes accompany this version. Known malformed input rejects
atomically; retained expression strings participate in resource limits.

## Reviewed measured-area source replacement (v30)

Boundary geometry edit envelope v6 adds required `replacement_linework_sources`
to the complete v5 envelope. It is a nonempty ordered array of edge source-use
arrays, with exact `owner_id`, `segment_id`, `parameter_start`, `parameter_end`
and `reversed` fields. It is permitted only for redefinition of an existing
measurement boundary derived from measured strokes. Wall-source replacement
cannot share this authority.

Admission proves that the replacement geometry and complete lineage identify
an actual current graph face in the owner's property/building/floor/layer and
active design phase. Presentation visibility does not change the source graph.
A face already assigned to another current area cannot be assigned twice.
The retained name, appraisal facts, classifications, factors and deductions
remain authoritative; deductions must fit the chosen face. Changed child
identities require explicit reference mappings or reviewed removals.

The redefinition archives its previous construction or topology and the v6
operation. The final lineage must agree with the latest retained source review
proof. Future source edits can make the reviewed area stale again; they cannot
rewrite that proof implicitly. Each affected owner is reviewed separately.

Native format 30 and extraction version 28 are required whenever this proof
exists, including Undo history, constraint-command boundary edits and imported
identified-boundary derivation archives. Prior edit dialects retain their
historical wire representation and format floors.
Measured strokes may also participate in design-phase registries. This new
membership requires native format 30/extraction 28 even without an area review,
including retained history. Generic vendor `model` properties do not qualify.

## Measurement linework model v1

`measurement_linework` is required 2D geometry, separate from architectural
walls and calculated measurement areas. Each entity represents one connected
stroke, open or closed. Its `properties` contain the usual `property_id`,
`building_id`, `floor_id` and `layer_id`, plus a typed `model`. All four IDs must
resolve to the same drawing context. `required` is true so an older application
that does not recognize the entity preserves the project read-only.

The model has `version: 1`, `replay_version: 1`, `stroke_id` (the entity ID),
`anchor: [x, y]` in metres, `closed`, ordered `segments`, and opaque `extensions`.
Each segment holds `segment_id`, `start_vertex_id`, `end_vertex_id` and `receipt`.
Receipts use the existing exact construction-input codec; original quantities
and angles remain retained. Geometry is reconstructed with individual receipt
replay, not the closed-boundary replay engine. There is no parallel writable
geometry cache. Consecutive edges share exact endpoint coordinates and vertex
identities. A reused vertex identity must retain its exact point. Closed strokes
end at their anchor and reuse the first vertex identity. Crossings and retracing
are allowed; they do not themselves establish valid area polygons.

Linework contributes no GLA or architectural wall volume. Classification and
calculation require separate derived area entities. Unknown positive model or
replay versions preserve the original model without guessing its geometry and
make the document read-only. Invalid known data rejects atomically.

Independent version-one strokes do not increase the SQLite envelope floor:
the required entity mechanism protects earlier readers. Canvas authoring,
dimensions, point jumping, analytical output and derived face detection use this
model. These implemented adapters do not establish certified Draw First parity.

## Measured-stroke geometry derivations (v29)

The recognized `(version: 3, replay_version: 3)` pair retains the original
`anchor`, `segments`, exact construction receipts and opaque `extensions`. It
requires an ordered `operations` array and forbids the version-two `transforms`
field. Each operation is exactly one of:

- `{ "type": "transform", "transform": <version-one rigid transform> }`.
- `{ "type": "edit", "edit": <stable geometry-edit intent>,
  "authored_length": <exact quantity or null> }`.

The edit intent uses the strict boundary geometry-edit codec, restricted to
`move_vertex` and `resize_segment` with the actual stroke identity. Irrelevant
fields, unknown operations, invalid values and redundant persisted edits reject.
An exact authored length uses the construction-receipt quantity codec and must
equal the resize intent's target length. Vertex edits cannot carry that quantity.

Replay reconstructs the original analytical stroke, then applies operations in
order. Moving a stable vertex updates every occurrence of that identity. Resizing
retains the chosen start or end point, chord direction and signed curve sweep;
connected mode translates all other vertices together. Original inputs are not
rewritten to impersonate the changed geometry. Subsequent rigid transforms act on
the edited world geometry. Precision loss, degeneracy and nonfinite results reject
atomically. Open, crossing and retraced strokes remain valid linework.

An effective API no-op retains the exact existing dialect. A real edit promotes
version one or two to version three, migrating prior rigid transforms into the
ordered operations without changing their order. Any recognized version-three
stroke anywhere in retained history requires native format 29 and extraction 27.
An old envelope cannot admit these semantics by lowering its version marker.
Other positive schema/replay pairs retain the existing opaque read-only policy.

## Rigid measured-line frames and live area sources (v28)

The recognized `(version: 2, replay_version: 2)` pair adds a required `transforms`
array. Every ordered operation has exactly `version: 1`, `pivot: [x,y]`,
`rotation_radians`, `flip_horizontal`, `flip_vertical` and `offset: [x,y]`.
All coordinates and angles are finite. The local anchor and original ordered
receipts stay unchanged; world geometry is obtained by replaying those receipts
then applying the operations in order. Effective identity operations preserve the
original version-one encoding. Scaling is not part of this rigid dialect.
Reflections reverse arc sweep. Operations that lose measurement precision or
collapse an edge are rejected. Other positive schema/replay pairs remain opaque.

Defined measurement boundaries retain `extensions.measurement_linework_sources`,
an array in boundary-edge order. Each edge has one or more source-use records:
`owner_id`, `segment_id`, finite `parameter_start` and `parameter_end` within
`0 <= start < end <= 1`, and boolean `reversed`. Qualification recomputes the
complete source layer's analytical graph and compares geometry and lineage.
Deleted, unsupported, context-mismatched or semantically hidden sources withhold
the dependent measurement and appraisal totals. Presentation visibility does not
change physical totals. Parent deductions require current sources too.

Canvas movement and rotation refresh previously current areas when their source
edge identities identify one unambiguous face with unchanged edge count. Boundary
IDs, edge IDs and classification facts are retained. Source and area edits occupy
one revision. A changed or ambiguous topology requires explicit area review;
the old boundary cannot continue contributing a qualified total.

Any retained recognized version-two stroke or identified measurement boundary
with linework lineage requires native format 28 and extraction version 26,
including deleted and undone history. Published older files containing
version-one strokes and lineage can still open without being rewritten; their
next save upgrades the envelope. This legacy-read exception does not permit a
version-two stroke inside a falsely lowered envelope. Original backup and
history-preservation rules continue to apply.

## Connected curved-wall rigid intent (v27)

Constraint-command envelope 10 retains the ordinary, boundary, wall, physical,
exterior-source and supplemental lanes. It adds explicit boolean
`source_completion`, `supplemental_source_completion` and
`supplemental_asset_reference_completion`. Every lane remains an array, including
empty lanes. False flags cannot carry their corresponding changes. Envelope 10
never carries corner-edit authority. Its retained discriminator requires native
format 27 and extraction 25 even when its selected wall lane is empty.

Selected curved-wall proof 4 has exactly `version`, `wall_id`, `baseline`,
`length_entry` and `rigid_transform`. The transform contains exactly `version: 1`,
`pivot`, `rotation_radians`, `flip_horizontal`, `flip_vertical` and `offset`.
Replaying the original analytical segment must reproduce the submitted baseline
exactly. The original construction input and earlier operations remain intact;
one rigid operation is appended and any existing exact length receipt is rebased.
Missing or substituted receipts, malformed transforms and mismatched baselines
are rejected. Proof 4 retains its curve-only contract.

Selected straight-wall proof 5 has exactly `version`, `wall_id`, `baseline`,
`length_entry` and `rigid_transform`, with `version: 5` and a straight baseline
whose `sweep_radians` is zero. Replay reconstructs source start to transformed
start and source end to transformed end, retaining the physical length. An
existing exact length receipt is retained or rebased without losing its exact
quantity, unit, rational value or opaque members. The explicit XY gradient
follows the transform's rotation and reflection, while pivot and translation do
not affect it, and legacy scalar rise fields stay synchronized. A reflection
may leave the baseline unchanged while changing a transverse gradient; that is
a real edit, while a complete no-op is rejected. The UI emits proof 5 for
straight walls with an explicit plane. The proof uses the existing rigid or
composed rigid command lane. Any retained proof 5, including one in a completion
receipt or history, requires native format 62. A rigid transform may exchange
named endpoint coordinates; ordinary endpoint-only reversal under older proofs
remains rejected. Proofs 1 through 3 retain their old wire and replay contracts.

Connected owners continue through existing endpoint solving. Only independently
verified selected rigid wall IDs receive the endpoint-coordinate exchange
exception; dependent owners retain ordinary reversal guards. Persisted hard
relations, analytical crossing topology, live exterior-source correspondence
and hosted-object validity still apply to the complete transaction.

Supplemental assets can use the same compact references as envelope 9, resolved
only against independently validated full assets from the result revision.
Envelope 10 requires the newer floor whether or not compact references are
present. Existing resource budgets and legacy envelopes 1 through 9 are unchanged.

## Compact compound asset history (v26)

Constraint-command envelope 9 retains the version-7 entity and exterior-source
lanes, with compact `supplemental_asset_changes`. An upsert has exactly `kind`
and `asset`; its asset reference has exactly `id`, `media_type`, `sha256`,
`byte_size` and `metadata_sha256`. The metadata hash covers the UTF-8 bytes of
canonical `metadata.dump()`. It binds metadata independently of the binary
content hash. An erase has exactly `kind` and `asset_id`. References never embed
binary bytes or metadata. The explicit envelope remains version 9 even with an
empty supplemental list; it cannot acquire version-8 corner-edit authority.

New compound measured-wall operations use this representation for changed
assets. Exact unchanged upserts and erases of absent assets need no supplement.
Legacy envelopes 1 through 8 keep their original wire and replay semantics,
including inline assets in envelope 7 and independent corner intent in 8.

Full asset payloads remain in the immutable result revision's `revision_assets`
rows. Native loading validates those rows before resolving envelope-9 references
against that same revision. A resolver is mandatory for an upsert. Missing
assets, mismatched IDs, media types, sizes, binary hashes or metadata hashes,
duplicate references, unknown fields and malformed records are refused. Existing
deterministic history replay compares the complete resulting entity and asset
state; a reference does not authorize an unrelated change.

Any retained envelope-9 marker requires native format 26 and extraction version
24, including Undo and deleted history and recovery archives. Both SQLite
markers and the logical digest retain that floor; recomputing a digest cannot
authorize downgrading it. Extraction publishes full assets separately and keeps
references in revision proof JSON. It remains an export, not a JSON importer.

The existing budgets are unchanged: 256 MiB per asset,
512 MiB aggregate persisted assets across retained history,
1 MiB per persisted JSON column and 64 MiB aggregate persisted JSON.
Compact history removes duplicated hexadecimal asset bytes from the bounded
proof; it does not remove document, archive or metadata admission limits.

## SVG component colors (v25)

An SVG symbol may retain optional `svg_palette` with exactly `version: 1`,
`profile: "white-outline-2"`, `outline_color` and `surface_color` in #RRGGBB.
Absence inherits the exact source appearance. An explicit default-valued palette
still records authored intent. Unknown profiles, fields, nulls, malformed colors
and palettes on procedural definitions are rejected.

Any explicit palette emits annotation state 7. Native format 25 and extraction
version 23 qualify typed annotation state 7 throughout retained Undo/deleted
history, including preserved version-7 records with no palette. Older files
keep their previous requirements. Both SQLite markers and the logical digest
retain the reader floor; a recomputed digest cannot authorize a downgrade.
Recovery archives preserve the same intent. Extraction is not a project importer.

The palette changes only a derived render copy. Pinned SVG bytes, artwork hashes,
definitions, geometry, transformations and opaque owner metadata remain intact.
The profile maps black outline strokes and primary white/light-gray surfaces,
retaining gradient shading, opacity and protected glass, recess and dark details.
The XML parser rejects unsupported active content, resources, roles and profiles.
Unsupported saved artwork is diagnosed and blocks incorrect output until reset
or explicitly updated. Canvas, sheet and PDF use the same palette-aware renderer.

## Saved-view drawing appearance (v24)

The typed `sheet_view_model` can retain optional `presentation.appearance` in
its coordinated views. A view is identified by its owning graph entity and
local view ID. Appearance has `visible`, optional `style`, and canonical
`objects`. Each object entry has a unique `object_id`, optional `style` and
optional `visible`. Null style inherits; null object visibility inherits.
The appearance property itself is omitted for complete inheritance and cannot
be null. Styles retain `outline_color`, `fill_color`, `fill_pattern`,
`line_width_mm` and `hatch_scale`; they contain no analytical geometry.

Absent appearance emits sheet/view model version 6. Explicit appearance emits
version 7 and requires native format 24 and extraction version 22, including
when the meaningful record exists only in Undo or deleted history. Explicit
values equal to current defaults still represent authored intent. Both SQLite
markers and the logical digest retain this minimum reader version; recomputing
a digest cannot authorize a lower format marker. Older files retain their
previous native format requirement. A preserved raw model-version-7 payload
also retains this reader requirement even if it has no appearance entries;
normal typed serialization emits version 6 for that case. Extraction remains an export, not a JSON
project importer.

Document admission validates object references even for unrestricted views.
Source filters may temporarily exclude existing targets without discarding their
appearance; deleting a target removes dependent entries in the same edit.
Global style is the fallback, followed by view style and then object style.
Local visibility overrides global appearance only, never organization or source
filters. Whole-view hiding suppresses the complete rendered scene. These
presentation changes preserve measured geometry and appraisal quantities.

## Explicit automatic-angle removal during redraw (v23)

Boundary redefinition intent v5 opts into `allow_automatic_angle_removal: true`.
It contains every v4 redefinition field, including `fresh_topology` as an explicit
boolean, `replacement_wall_source_ids`, child mappings and removed-reference
IDs. The fresh flag may be false when the segment count changes. Other edit
kinds cannot carry this policy; the removal list must be nonempty.

Live application independently requires at least one listed affected automatic
angle dimension. Unrelated or unknown IDs, automatic edge lengths, area
dimensions and unused permission are refused. Automatic edge measurements
regenerate normally; kept angle targets require explicit incident edge/corner
mappings. Nothing is removed merely because an old corner disappears.

The normal redraw finish archives `desktop_operation` v3: the exact v2 target,
map, removal list and replacement-segment hash, plus the true policy flag.
The typed command must match that full archive. Removing, changing or
downgrading the flag cannot authorize another finish or survive archive replay.
Undo restores the original annotation exactly; Redo reapplies the same decision.

Any retained v5 intent requires native format 23 and extraction format 21,
including undone/deleted revisions or imported geometry-derivation evidence.
Both SQLite format markers and the logical digest retain that reader floor.
Extraction preserves the complete versioned intent and history; it remains
an extraction format, not a JSON project importer. Existing v1-v4 intent
encodings and their original removal rules remain unchanged. Older files
without this policy retain their previous required format.

## Original typed wall input

A physical wall created with exact drawing input can retain
`properties.original_drawing_input`, encoded with the existing standalone
ConstructionReceipt codec. Inline cardinal input uses `line_rise_run`; the
precision form also supports headings, relative turns, world coordinates and
the four analytical arc constructions. The receipt retains entered Quantity
and AngleInput expressions. Its segment ID is the initial wall ID. Creation
replays the receipt and requires its endpoints and sweep to match the new
authoritative baseline exactly.

Precision wall input also retains `properties.original_drawing_input_context`:
`version: 1`, `expected_start` as a two-element model-metre array,
`previous_segment` as the original canonical baseline or null,
`closure_anchor` as a two-element model-metre array or null, and
`tolerance_metres`. Only relative-turn receipts retain a `previous_segment`;
only closure receipts retain a `closure_anchor`. Other receipts store null
for these unused fields. This preserves the ending tangent needed to
replay a relative turn after a straight or curved wall. It is a historical
context, not a live dependency or a replacement for persistent endpoint
constraints. Older inline walls with a rise/run receipt need only its start
and the documented geometry tolerance to replay their original input.

This is historical input provenance. The wall's current `baseline` remains the
geometry authority; later moves or dimension edits can differ from the original
receipt. Readers must not substitute replayed original input for current
geometry. It is ordinary optional preserved metadata, requires no new project
format version and survives native history and save/reopen.

Reusable text templates use a separate [local text-library format](text-library-format.md).
Placed labels store their complete content and style in the project; opening or
editing them does not require that library file.

Annotation state version 5 adds optional label `model_plan: true`: its position
is a world XY anchor rendered only in horizontal plans, projected through each
plan frame. Rotation remains view-relative for readable, consistent text.
Absence keeps the legacy view-overlay convention. The field is refused in
versions 1–4; old readers refuse version 5 rather than silently misplacing it.
Version 5 retains the version 4 area-presentation and version 3 symbol schemas.
Encoding uses version 6 for wall measurements, otherwise version 5 when any
plan-anchored label exists, version 4 for area placement overrides, otherwise
version 3. Raw edits may retain a higher known
version after its last optional record is removed. The SQLite layout is unchanged.

## Derived wall measurement presentation

Annotation state v6 adds presentation overrides with `target_kind: "wall_dimension"`
and `target_id` equal to a wall's stable ID. The displayed value is always the
current analytical baseline length; no copied numeric measurement is stored.
`visible` suppresses only the measurement, never the wall or its openings.
Style controls text color, font and emphasis. `inherit_appearance: true` retains
the derived theme appearance for placement/visibility-only edits.

Optional `plan_label_offset_m: [x, y]` is a finite world XY offset from the
analytical midpoint. It follows that anchor as wall geometry changes; absence
restores automatic exterior placement. Optional `paper_text_height_mm` is a
finite positive value at most 100 mm (the editor permits 0.5–20 mm).
Optional `plan_label_rotation_radians` is a finite world-plan angle; absence
retains the upright wall-derived angle. A horizontal named view projects the
anchor, leader and angle. Pointer placement is inverse-projected before saving.
Wall measurement records cannot override wall outlines or hatches through
`paper_line_width_mm` or `hatch_scale`.

The target kind and new fields require v6. That version retains v4 area offsets,
v5 authored plan labels and v3 symbol transforms. Earlier readers refuse v6;
earlier states without wall measurements keep their previous encoding versions.
Raw updates preserve unrelated records, pinned artwork and opaque metadata.

## Exterior measurements derived from walls

An identified measurement boundary may carry `properties.wall_measurement_source`
with exactly `version: 1`, `basis: "exterior"` and `walls`. Each wall record has
exactly a stable `id` and a `context` object. Context may record nonempty
`property_id`, `building_id`, `floor_id`, `layer_id` and `phase_id` strings.
Records are unique and stored in ID order; they identify the complete perimeter
walls, excluding interior partitions and branches.

The boundary stores analytical line and circular-arc segments in the ordinary
identified-boundary format. Its construction receipts retain those curves.
The source record does not cache numeric area or perimeter. Currentness derives
the exterior again from current wall baselines and thicknesses, then checks
context and the complete analytical outline, including signed curvature.
Missing sources, changed geometry, malformed provenance or manual outline edits
withhold qualified appraisal quantities until repaired or refreshed. Openings
do not change this exterior outline. Refresh uses the recorded perimeter wall
IDs; an addition with different perimeter membership needs a new measurement.

## Unfinished Auto-Subtract drawing

An unfinished drawing with an explicit parent uses active recovery envelope
version 2, replay version 1. It adds the required nonempty
`auto_subtract_target_id` to `version`, `replay_version`, `source`, `checkpoint`
and `extensions`. The checkpoint and workspace-history schemas remain version 1.
Ordinary drawings retain their exact five-field active version 1 envelope.
Auto-Subtract and the desktop redraw operation cannot be combined. The chosen
parent participates in lifecycle/finish replay, retired-input validation and
resource accounting; it is not an inferred parent or a desktop-only annotation.
Unknown positive active versions remain opaque and make the owning recovery
history read-only. Completed areas use the existing parent `deduction_ids` field;
the final commit includes both new entities and the changed existing parent.

## Circular column selection orientation

Circular columns retain an optional finite `rotation_rad` property in radians.
It records the orientation of the selection frame; the cylinder's physical
geometry is rotationally symmetric. Older columns without this property read
as zero. Transform commands update it, while dimensional and property edits
preserve it. Undo/redo and project history retain the value alongside geometry.

## Symbol instance transforms

Area presentation overrides may carry `plan_label_offset_m: [x, y]`, two
finite model-space metre offsets from the owner's derived plan-label anchor.
An area uses `target_kind: "area"` for this field. It controls the placement of
derived names and quantities; it does not store a numeric area value or change
geometry. An explicit zero offset is a manual centered placement. Absence
restores automatic placement. Annotation state version 4 or later is required when an
offset is present; versions 1–3 cannot admit it. Placement-only records may
also carry `inherit_appearance: true`, so positioning
a label retains semantic colors, fills and linework. Explicit appearance edits
remove this flag. Version 4 retains the version 3 symbol representation. States
without offsets, appearance inheritance, plan-anchored labels or wall measurements encode as version 3,
and future unknown versions remain unsupported.

Area presentation overrides optionally carry `paper_line_width_mm` (finite,
0.05–10.0) and `hatch_scale` (finite, 0.1–10.0). Both fields are optional within
annotation state version 3: legacy records retain their existing defaults and
encode without the fields when absent. These values affect presentation only;
they do not change analytical geometry or area calculation facts. Desktop area
appearance edits preserve unrelated raw annotation records and metadata.

Annotation state version 3 adds `width_scale`, `depth_scale`, `flip_horizontal`
and `flip_vertical` to each saved symbol. Dimensions equal the saved definition's
physical width/depth multiplied by `placement.scale` and the corresponding axis
factor. Mirroring and axis scaling act about the saved definition anchor before
rotation and translation. Factors must be finite, positive and within the saved
definition's effective scale limits. Rotation remains a model-space radian angle.
Version 1 and 2 annotations migrate with axis factors of one and both flips false;
their pinned definition and exact SVG bytes remain unchanged. Labels retain their
existing placement model. Each resize, flip or rotation is a normal undoable
document command, and rendering and export consume the same saved transforms.

Vertex projects are standalone SQLite files containing one immutable logical
document snapshot and the complete command history known when that snapshot was captured.
The file is an interchange/save artifact. The current foundation keeps the working document
in memory; it does not claim to be a live SQLite working journal.

The bundled `vertex-cli` provides local format operations without a hosted service:
`inspect` reports document identity, revision, entity/asset counts, history, and editability;
`validate` loads and checks the storage, structural references, logical digest, and asset bytes;
`extract` writes a new JSON-and-assets directory; and `migrate <source> <destination>` loads a
supported project and writes a validated copy at the current storage version. Migration refuses
an existing destination, reports source and destination SHA-256 fingerprints, and verifies that
the source hash is unchanged. A failed migration leaves the source and any existing destination
untouched.

These CLI operations also recognize supported ordinary and recovery-copy
archives. Inspection reports their role and recovery record counts; validation
checks the archive and its replayable recovery state. Migration preserves the
complete ledger and its role through `save_archive`, rather than saving only
the document. Extraction writes exchange version 12 with the complete raw
recovery records alongside document history and assets. The SQLite format
version has a v4 minimum for recovery-bearing archives; retained document
proofs can require a higher version.

An unknown recovery kind or version can be inspected only as an opaque archive:
the CLI reports its source fingerprint, diagnostic and record metadata with
`editable: false` and `validation_complete: false`. It does not expose a
forkable document snapshot. Validate, migrate and extract refuse that state
before creating an output. Damaged storage or a failed digest remains an error.

Retained typed `ApplyBoundaryConstraintChanges` endpoint commands replay through the same analytical
topology checks used by interactive authoring. A matching command/result pair
and recomputed logical digest cannot authorize new undeclared wall contacts,
crossings, overlap or reversal of a protected closed loop. Unsafe history
rejects with an integrity diagnostic; the loader preserves the original bytes.
Ordinary explicit construction/transform commands retain their separate policy.
New straight wall-only endpoint authoring retains this typed intent. Historical
straight wall-only authoring stored as generic `ApplyEntityChanges`
does not encode endpoint-edit intent and is not covered by this typed replay
policy; its missing intent is not inferred retrospectively.

Version 2 retains the v1 table structure and adds a mandatory compatibility
boundary for identified geometry. Any identified boundary, boundary draft or
dimension in retained history requires v2, including an undone or deleted
identified boundary. Both SQLite `user_version` and `metadata.format_version`
must agree, and the logical digest includes that version. The reader accepts
v1 legacy history, v2 identity history, v3 construction-receipt history, v5
translation history, v6 transform history, v7 boundary-coordinate edit
history, v8 boundary-constraint transactions, v9 measured group translations,
v10 curved endpoint constraints, v11 straight wall-only intent, v12 physical
arc-length locks, v13 direct physical curve-length inputs, and v14 rigid curve
construction transforms, v15 fixed-chord boundary curvature edits, v16
reviewed exterior wall-source replacements, and v17 explicit fresh-topology
boundary replacements, v18 measured group rigid transforms, and v19 coordinated
physical wall/exterior measurement changes, and v20 mixed ordinary object/asset
and exterior changes, v21 ANSI-oriented appraisal evidence, and v22 coordinated
exterior-corner edits, v23 explicit automatic-angle removal during redraw,
v24 saved-view drawing appearance, v25 SVG component palettes, v26 compact
compound asset references and v27 connected curved-wall rigid proofs, plus v4 through v27 archives through
recovery-aware APIs.
Under-versioned semantic data and versions above 27 reject. Legacy-only history
is still written as v1. Unknown boundary entity
versions in v2 remain preserved read-only. See `boundary-entity-format.md`.
Version 2 also recognizes `dimension` entities. Segment-length dimensions refer
to one stable child ID; angle dimensions refer to two stable child IDs and their
shared vertex; area dimensions refer to the complete identified closed boundary.
Every retained state validates the supported references. Unknown dimension
versions and kinds remain opaque and make the project read-only. A supported
dimension on an unknown boundary version is preserved read-only without
guessing its geometry.
See `boundary-dimensions.md` for the typed dimension contract.

Version 3 retains the v1 and v2 tables and adds a storage guard for the reserved
`boundary_authoring` property. The guard first qualifies an explicit identified
boundary: a recognized boundary type with supported integer
`boundary_model_version: 1`. A `boundary_authoring` property on that owner
requires v3 anywhere in retained history, including an undone or deleted
entity, regardless of the envelope version or shape. Generic entities and
anonymous legacy boundaries may retain a vendor collision in v1. An unknown
positive boundary model remains v2 and opaque. The property is preserved as
opaque JSON through save and load; receipt envelope validation, replay
semantics, and editability belong to the document and receipt codec layers.
See `boundary-authoring.md` for the authoring contract.

## Boundary construction envelopes v1 and v2

On a supported identified boundary, `properties.boundary_authoring` has exactly
these fields: `version` (1 or 2), `replay_version: 1`, `boundary_id`, `anchor`,
`segments`, and `extensions`. `anchor` is a finite `[x, y]` point in metres;
`boundary_id` equals the owning entity ID. `extensions` is an opaque JSON
object. Both known schemas implement replay algorithm version 1. An unknown
positive replay version preserves the complete envelope and makes the retained
document read-only, as does an unknown positive schema version. Missing, zero,
negative or noninteger version fields reject for recognized schemas. Unknown
schemas remain opaque without assuming their payload shape.

`segments` is a nonempty ordered array. Each member contains exactly
`segment_id`, `start_vertex_id`, `end_vertex_id`, and `receipt`. Its identities
must match the corresponding canonical boundary segment. The receipt repeats
`segment_id` and contains `kind`, `start`, `clockwise`, and the fields below.
There are no optional or additional fields for a known kind.

| `kind` | Additional required fields |
| --- | --- |
| `line_heading` | `distance`, `heading` |
| `line_rise_run` | `rise`, `run` |
| `line_relative_turn` | `distance`, `turn` |
| `line_closure` | `closure_delta` |
| `arc_chord_angle` | `chord_end`, `angle` |
| `arc_chord_height` | `chord_end`, `height` |
| `arc_chord_length` | `chord_end`, `arc_length` |
| `arc_start_tangent` | `tangent`, `arc_length`, `sweep` |

Schema v2 adds `line_to_point`, requiring only `chord_end` beyond the common
receipt fields. Schema v1 remains closed to the eight kinds above and rejects
`line_to_point`. Current authoring sessions emit schema v2, while older schema
v1 records retain their version when re-encoded. The SQLite storage version
remains 3 for either envelope.

Receipt schema v3 adds a required `transforms` array. Each entry has exactly
`pivot`, `rotation_radians`, `flip_horizontal`, `flip_vertical`, and `offset`.
Points are finite two-number arrays, rotation is a finite number, and flips are
Booleans. Replay first reconstructs the original local receipts using schema-v2
rules, then applies each transform in order: rotate about the pivot, reflect X
and Y about the pivot as requested, and translate. An odd number of reflections
reverses arc sweep. Each resulting boundary must remain valid. Replay rejects
non-finite residuals, endpoint or analytical-length drift beyond its geometry
tolerance, and accumulated pure-translation rounding beyond that tolerance;
see `geometry-operations.md` for the precision contract.

The stored anchor, receipt coordinates, closure vectors, exact quantities, and
entered expressions remain local and unchanged. Replayed edges and anchor are
world coordinates; replayed receipts still contain their original local inputs.
Copies may remap typed identities. Extensions are preserved without interpreting
identifier-shaped user data. An empty transform array is valid. Schema v1/v2
reject the transforms field and retain their original encodings; new drawing
sessions still emit v2. The SQLite receipt storage minimum remains format 3;
explicit in-place translation history independently requires format 5.
Explicit in-place rotation/reflection history requires format 6.
Direct stable-ID vertex moves and segment-length changes require format 7.

Point construction copies the finite endpoint directly into a straight segment
after checking its exact start and minimum chord length. It performs no angle
conversion or synthetic quantity parsing. It records a coordinate-defined edge;
it does not establish click origin, a typed measurement, a snap relationship or
a geometric constraint. Snapping resolves coordinates before this command.

Points and deltas are finite two-number arrays. `clockwise` is a Boolean; it
must be false except for chord-length construction, whose unsigned length
needs this direction choice. Signed angle, height or sweep inputs carry
direction for the other arc forms.

A quantity object has exactly `metres`, `exact_metres`, `entered_unit`, and
`original_expression`. `exact_metres` contains signed 64-bit `numerator` and
positive signed 64-bit `denominator`. `entered_unit` is one of `metre`,
`millimetre`, `centimetre`, `foot`, or `inch`. Parsing the nonempty expression
with that default unit must reproduce the stored quantity exactly.

An angle object has exactly `radians`, `original_expression`, and
`normalized_expression`. Both nonempty expressions must parse to the stored
finite radians without a tolerance. The normalized expression is the canonical
round-trip decimal radians string produced by the v1 codec.

Replay derives each edge from these inputs, using the preceding edge for a
relative turn and the initial anchor for generated closure. It must reproduce
the canonical ordered topology and analytical coordinates exactly, and the
result must be a valid closed boundary. Receipts document a reproducible
construction; they do not prove that a person entered an expression. Display
rounding never changes these stored values.

## Document contract

### Typed command envelopes

Application edits cross the workspace boundary as typed JSON command envelopes.
Ordinary edits use version 1; later envelopes retain the specialized geometry,
source-completion and asset-reference intent described below.
`sketch::command_to_json` and `sketch::command_from_json` preserve
the command kind, expected revision, entity and asset changes, quantities,
metadata, and boundary transforms. Known envelopes reject unknown fields,
unsupported versions, invalid identifiers, non-finite coordinates, malformed
asset hex, and asset digest mismatches before a command can be applied.

The supported `kind` values are `apply_entity_changes`, `name_revision`,
`translate_boundary`, `translate_boundaries`, `transform_boundary`,
`edit_boundary_geometry`, and `apply_boundary_constraint_changes`. An apply envelope contains
typed `entity_changes` and `asset_changes`; an upsert carries the complete
entity or asset payload and an erase carries its stable ID. Assets use a
lowercase `bytes_hex` representation and retain their SHA-256. Translation
and transform envelopes carry explicit finite `offset`, `pivot`, rotation, and
reflection fields. Boundary geometry envelopes carry the strict edit intent
described above. `ProjectWorkspace::prepare` round-trips each command
through this codec on an isolated immutable fork before staging one document
revision, so one accepted compound operation has one undoable history entry.

Every semantic entity has a stable ID, a type, a JSON `properties` object, a `required` flag,
and a JSON `extensions` object. IDs are document identity and are never derived from geometry.
The v1 known types are:

`property`, `building`, `floor`, `layer`, `boundary`, `measurement_boundary`, `room_boundary`,
`wall`, `wall_join`, `opening`, `room`, `slab`, `roof`, `roof_join`, `stair`, `railing`, `column`, `beam`, `label`, `sheet`, `view`,
`constraint`, `reference_grid`, `terrain_surface`, `dxf_source`, and `ifc_source`.

All geometry properties use metres and radians. A wall and opening can be represented as:

The root `property` entity may persist `calculation_workflow` as `"measurement"`
or `"appraisal"`. Its `calculation_profile` contains `id`, positive `version`,
`display_unit`, `decimal_places`, and a `classifications` object. Every
classification rule stores boolean `building_total` and `living_total` values
plus an `appraisal_category` token. Supported appraisal tokens are `none`,
`above_grade_finished`, `above_grade_unfinished`, `below_grade_finished`,
`below_grade_unfinished`, `garage`, `carport`, `porch`, `patio`, `deck`, and
`other_non_living`. Appraisal workflow uses the versioned `vertex-appraisal`
profile; a prior measurement profile is retained separately as
`measurement_calculation_profile` so switching workflows is reversible. Missing
workflow or appraisal-category fields migrate to measurement and `none`, which
prevents an older project from silently acquiring GLA classifications.

Appraisal display uses only `calculation_profile.decimal_places` (integer 0–6,
default 2) and the current workspace unit. Its saved `version` is a positive local
display-configuration revision, separate from the fixed appraisal policy version.
An Area display edit updates only `decimal_places` and `version` when the profile
object exists, retaining other fields and extension metadata verbatim. New
configuration objects use the built-in profile defaults. This uses existing
property metadata; no SQLite format migration is required. Malformed decimal
settings visibly withhold numeric appraisal reports. A malformed or exhausted
configuration revision prevents a display edit without changing geometry or
qualifying/withholding a report that has valid decimal settings.

Generated appraisal schedule cells carry optional `display_decimal_places`
presentation metadata. Their `ScheduleQuantity` values remain unrounded SI;
the metadata is rebuilt from the property and is not a stored schedule format.

Closed area entities retain workflow-specific meanings independently.
`measurement_classification` stores the user-selected measurement rule and
`appraisal_category` stores the explicit appraisal category. `classification`
remains the compatibility measurement value. Projects written by the initial
appraisal preview, which placed an appraisal token in `classification`, are
recognized and migrated on a workflow change. An area created for the first
time while Appraisal is active receives the neutral `measurement` rule when the
user later enters Measurement; Vertex never infers an appraisal category from
its name, floor, or geometry.

```json
{
  "id": "wall-1",
  "type": "wall",
  "required": false,
  "properties": {
    "floor_id": "floor-1",
    "baseline": {
      "start": [0.0, 0.0],
      "end": [5.0, 0.0],
      "sweep_radians": 0.0
    },
    "thickness_m": 0.14,
    "height_m": 2.4,
    "elevation_m": 0.0,
    "slope_rise_m": 0.3,
    "layers": [
      {"id": "outer", "thickness_m": 0.02},
      {"id": "core", "thickness_m": 0.10,
       "material_assignment": {"version": 1, "catalog_id": "assemblies", "material_id": "brick"}},
      {"id": "inner", "thickness_m": 0.02}
    ]
  },
  "extensions": {}
}
```

`properties.layers` is optional. When present it is an ordered, contiguous
wall assembly from the negative to positive side of the wall baseline normal.
Each layer has exactly `id` and `thickness_m`, plus an optional
`material_assignment` object with exactly `version: 1`, `catalog_id`, and
`material_id`. Layer IDs are stable document-local identifiers; thicknesses are
positive metres and must sum to the parent `thickness_m` within the document
precision tolerance. A layer material points to an `assembly_model` entity and
its cataloged material. The same hosted opening geometry is cut through every
layer, while the layer stack remains available for schedules and future
assembly editing. An empty array is equivalent to a monolithic wall.
An optional signed `slope_rise_m` (or legacy `slope_rise`) gives the change in
top height from `baseline.start` to `baseline.end`, while the bottom stays at
`elevation_m` and `height_m` is the top height at the start. Without an explicit
`top_plane`, this rise defines a chord-aligned grade, including on curved
baselines. Native v62 also admits the explicit planar gradient described above,
which can include a component across the wall. Hosted openings must fit below
the local minimum top over their span and the full wall thickness.

A `wall_join` is a version-1 architectural relationship that preserves the
source wall entities while providing one derived fused solid for coordinated
views. Its properties contain exactly `version: 1`, `style: "fused"`, and a
`wall_ids` array of two to thirty-two unique wall IDs. Every referenced wall
must exist and be a `wall`; one wall may not belong to more than one join. The
native geometry builder additionally requires every member to share a
connected endpoint with another member and to have overlapping vertical
extents. The fused shape is a derived cache: source wall dimensions, hosted
openings, classifications, and schedule quantities remain authoritative and
are never replaced by the join record.

An opening may carry an optional `properties.opening_assembly` object. The
object is exactly:

```json
{
  "version": 1,
  "kind": "door",
  "frame_width_m": 0.08,
  "frame_depth_m": 0.12,
  "panel_thickness_m": 0.04,
  "glazing_thickness_m": 0.0,
  "inset_m": 0.0
}
```

`kind` must match the opening's `opening_kind` (`door` or `window`). All
dimensions are finite metres; frame and panel dimensions are positive, glazing
is nonnegative, and the panel cannot be deeper than its frame. Window profiles
require positive glazing. `inset_m` is signed toward the wall's left-hand
normal and the profile must fit within the host wall thickness. The profile is
an instance presentation contract: the wall cut, hosted dimensions, and
handing remain the source of truth, while frame, leaf/sash, and glazing solids
are derived for coordinated views.

For windows, profile version 2 contains those same seven fields and exactly
five additional fields: `window_layout` (`fixed`, `double_fixed`, `triple_fixed`,
`casement`, or `sliding`), boolean `window_hinge_at_end`, boolean
`window_open_left`, numeric `window_angle_degrees` in [0,180], and numeric
`window_slide_fraction` in [0,1]. Casement uses the jamb, side and angle; sliding
uses the moving half, track side and fraction. Casement requires zero slide
fraction; sliding requires the canonical angle 90. Fixed layouts require the
canonical movement fields (`false`, `true`, `90`, `0`). Canonical single-pane
profiles serialize as version 1. Version 2 is window-only. Missing or extra
fields, unsupported layouts and dormant conflicting movement values reject.
Both moving layouts currently require a straight host. Split fixed panes
preserve curved-host geometry. Their manufactured parts, plans and exchange
derive from this profile; the original hosted wall cut remains authoritative.

Bay windows use profile version 3: exactly the twelve v2 fields plus
`window_bay_projection_m` and `window_bay_front_fraction`. Only a window with
`window_layout: "bay"` may use v3, and bay layouts cannot use v2. Projection is
positive, finite, bounded to 10 metres, and measured beyond the selected host
wall face. The front fraction is strictly between zero and one and describes
front-face width relative to the full wall-opening width. Geometry admission
also requires room for its mitered frames and glazing. `window_open_left`
selects the projecting side relative to the host direction; bay hinge, angle
and travel remain `false`, `90`, and `0`. Non-bay profiles cannot carry dormant
bay dimensions. Earlier v1/v2 shapes and serialized fields remain unchanged.

A bay is a fixed, directly glazed assembly with three frame facets, sealed
top/bottom plates and mounting shoulders. Its existing panel-thickness field
is retained for profile compatibility and bounds glazing thickness; it does
not describe a separate operable sash. A bay alone adds no floor or appraisal
measurement area. Curved-host bay geometry remains a required unimplemented
feature; current admission explains the straight-host requirement.

An opening may also carry `properties.door_operation`. Version 1 has exactly
`version: 1`, `hinge: "start" | "end"`, `side: "left" | "right"`, and a finite
`angle_degrees` in (0,180]. Version 2 adds exactly `kind` (`hinged`,
`double_hinged`, or `sliding`) and numeric `slide_fraction` in [0,1]. Only a
slider may have nonzero travel. Ordinary hinged operations serialize in the
original version-1 form. Unknown, missing and extra fields are rejected.
The slider's hinge identifies the movable half-panel's jamb, side chooses its
track, and fraction one stacks it behind the fixed half-panel. A profiled
slider requires a straight host and sufficient frame depth for two tracks.
Double doors retain two opposing jamb leaves and two analytic swings. Profiled
double admission rejects leaf/frame/host intersections at the requested pose.
These records remain dormant when the opening is classified as a window;
reclassifying it as a door restores their use. Detailed physical and legacy
limits are documented in [hosted openings](hosted-openings.md).

A `roof_join` is a version-1 architectural relationship that preserves the
source roof entities while providing one derived fused solid for coordinated
views. Its properties contain exactly `version: 1`, `style: "fused"`, and a
`roof_ids` array of two to sixteen unique roof IDs. Every referenced roof must
exist and be a `roof`; one roof may not belong to more than one join. The
native geometry builder additionally requires every source solid to touch at
least one other member. Source roof parameters, openings, materials, and
schedule quantities remain authoritative and are never replaced by the join.

```json
{
  "id": "opening-1",
  "type": "opening",
  "required": false,
  "properties": {
    "wall_id": "wall-1",
    "offset_m": 1.2,
    "width_m": 0.9,
    "sill_m": 0.0,
    "height_m": 2.0
  },
  "extensions": {}
}
```

A slab uses `boundary`, an array of the same `{start,end,sweep_radians}` segments; `holes` is
an array of boundary arrays. Its scalar fields are `thickness_m` and `elevation_m`. An optional
`element_kind` is one of `slab`, `floor`, `ceiling`, or `foundation`; the four kinds share the
same geometry and quantity rules. An optional `layers` array uses the same strict layer object
shape as walls (`id`, `thickness_m`, and an optional version-1 `material_assignment`) but orders
layers from the lower surface to the upper surface. Layer thicknesses must be positive and sum
to the parent `thickness_m` within the document precision tolerance. Each material assignment
resolves to an `assembly_model` catalog and is retained for per-layer quantities and schedules.
The native solid reader and all plan, elevation, section, and 3D projections decode this stack;
each layer receives the same boundary holes at its own elevation so the compound volume remains
the authoritative sum of the layer solids.
Entered unit text and exact quantity fields are separate semantic properties; display units do
not change the metre geometry.

An architectural room volume uses a `room` entity with `boundary` (or the
migration-compatible `segments`) and an optional `holes` array of boundary arrays.
It requires positive `height_m` and finite `elevation_m`; all values are stored in
metres. The same analytical room volume is used by plan, elevation, section,
native 3D, and quantity consumers. `room_boundary` remains a separate 2D
appraisal/space boundary type. A legacy `room` row without height or elevation
can still be displayed as a plan boundary, but solid-driven views report the
missing volume fields instead of inventing a default height.
Complete room volumes are validated when created, changed, or restored from a
native project, including historical revisions. Invalid dimensions, malformed
boundaries, and outside, touching, intersecting, overlapping, duplicate, nested,
or numerically indeterminate holes are rejected before loading succeeds. This
uses the shared analytical boundary validator without requiring a 3D renderer.
Legacy `segments`, `height`, and `elevation` aliases remain accepted; canonical
fields take precedence when both forms are present.

Floors may carry an optional version-1 `vertical_level_binding` object with exactly
`graph_id` and `level_id` (plus `version: 1`). `graph_id` resolves to a `vertical_levels`
entity and `level_id` resolves inside that graph. The Document validator admits the binding
only on floors and rejects a missing graph, wrong graph type, malformed model, or missing level.
Removing the property clears the association without changing world-coordinate geometry.
See [explicit vertical levels](vertical-levels.md).

Walls, slabs, and architectural objects may also carry an optional version-1
`vertical_placement` object with `mode` (`"level"` or `"absolute"`) and a finite
`offset_m`. New objects authored on a floor with a level binding default to
`mode: "level"`; projection resolves the bound elevation plus the offset from
the current snapshot without rewriting the stored coordinates. Absolute mode,
including an omitted placement record, preserves source elevation for imported
or explicitly fixed geometry. The resolver is shared by plan, elevation,
section, and native 3D output and rejects malformed or unbound level requests.

Reference grids are optional `reference_grid` entities. Their `properties.model`
is a strict version-1 object containing `origin_m`, `rotation_radians`,
independent X/Y spacing and extents, `major_every`, axis labels, and `visible`;
all geometry is stored in metres/radians. The model is validated at the
Document boundary and rendered by both desktop canvases from the same line
list. Grids are presentation aids only: they do not participate in area
totals, wall geometry, or measurement truth. See
[reference grids](reference-grids.md).

Terrain surfaces are optional `terrain_surface` entities. Their `properties.model` is a strict
version-1 local triangulated irregular network (TIN) with `provenance`, bounded `points`,
`triangles`, `contour_interval_m`, and `visible` fields. Each point stores a stable `id`,
`x_m`, `y_m`, and `elevation_m`; triangle indices refer to that point array and each edge may
belong to at most two triangles. The model is the measurement authority for the surface:
plan edges and contour segments are derived from it, while native 3D uses the same triangles
as OCCT faces. A terrain entity carries the ordinary property/building/floor/layer placement
links when it belongs to a drawing context. The source boundary and any entered elevation
expressions are retained as authoring metadata; editing the terrain never rewrites the
source boundary. Terrain remains a local surface tool and does not imply a survey provider,
georeferencing service, native Apex compatibility, or production qualification. See
[terrain surfaces](terrain-surfaces.md).

An imported DXF may retain its original bytes as an asset referenced by a
`dxf_source` entity. The entity records the source filename, declared DXF
version, mapped candidate count, and stable fidelity diagnostics; it is a
provenance record and is not drawing geometry. Editable candidates keep their
source layer and primitive in `extensions.dxf_source`. Removing or replacing
the source record never changes the mapped geometry.

Hosted openings always retain `wall_id` as their physical host. Without an
explicit `layer_id`, their drawing context and hierarchy parent inherit the
wall. An explicit valid layer on that same property/building/floor may provide
a separate drawing layer; if it differs from the wall's layer, that layer is
the opening's hierarchy parent and visibility context. The physical floor,
level, station and world elevation still come from the wall. Missing,
malformed, unresolved or different-floor layer references remain diagnostics;
they never establish another physical host or shift the opening.

The desktop source receipt additionally records `layer_reviewed` and a
`layer_mapping` array. Each entry contains `source_layer`, destination
`floor_id` and `layer_id`, `created_layer`, and `editable_item_count`. These
fields are provenance; editable entities use their ordinary floor/layer
properties and annotation children use `placement.layer_id`. New destination
layers, geometry, annotations and retained source are committed together.
The mapper's intermediate annotation entity carries
`extensions.dxf_annotation_layers`, an object from reconstructed child IDs to
effective CAD layer names, so labels and dimensions can be routed before
annotation merging. Native wall/opening candidates also retain their INSERT
layer in `extensions.dxf_source`. This opaque metadata does not change the
native container version or historical command interpretation.

An imported IFC may retain its original STEP bytes as an asset referenced by an
`ifc_source` entity. The record follows the same provenance shape as
`dxf_source`, with IFC4 format, source filename, mapped candidate count, and
stable fidelity diagnostics; mapped candidates retain an `extensions.ifc_source`
record identity/type.

For known entity types, the document validates `refs` and `references` arrays as generic entity
references. It also validates canonical singular and plural reference fields for each known type,
such as `property_id`, `building_id`, `floor_id`, `wall_id`, `column_id`, `beam_id`, and `sheet_id`;
a referenced entity must exist and have the named type. `boundary_id` continues to mean the exact
`boundary` type; the distinct `measurement_boundary` and `room_boundary` types carry their own
`floor_id` and `layer_id` links. `parent_id`, `host_id`, `target_id`, `entity_id`, and
`source_entity_id` are generic
references. `asset_id` and `asset_ids` resolve against the pinned asset map; a missing asset or
deletion of an asset still in use rejects the complete command. This is structural referential
validation for ordinary entities. Known persisted wall constraints additionally enforce their
semantic bindings, hard residuals and host validity at the document boundary; see
[persistent wall constraints](constraint-entity-format.md). Full geometric, topology and solver
workflow validation remains incomplete.

Commands are the only mutable interface. `ApplyEntityChanges` atomically applies entity and asset
upserts/deletes against an exact expected revision. Duplicate operations, stale revisions,
invalid JSON, asset checksum failures, and dangling or mistyped references reject the whole
command without advancing the revision. Persisted action text is limited to 1,024 bytes and
revision names to 256 bytes; both require valid UTF-8 without embedded NUL. Media types have the
same encoding requirement and reject CR/LF. JSON floating-point NaN and infinity are rejected
before encoding because JSON cannot preserve them. `NameRevision` records an immutable named
revision.

Undo and redo create new, monotonically increasing revisions. An edit after undo clears the
navigation redo stack but retains every old revision and named branch in history. Each revision
currently stores a full entity and asset state. This is intentionally simple and lossless, but
large histories can use substantial space; delta compaction is a future format change.

`DocumentSnapshot` owns copies of every entity, history record, and asset byte. Its getters are
const-only. Saving a snapshot captured at revision R always writes R, even if the working document
has advanced to R+1. After publication, `mark_saved(R)` leaves an R+1 head dirty.

Unknown optional entity types and unknown JSON fields survive unrelated commands and a v1
save/load cycle. Property and extension JSON is encoded deterministically by nlohmann JSON, so
v1-generated unknown values re-encode with the same JSON value types and canonical bytes. Input
whitespace, source object-key order, and alternate numeric spellings are not retained. An unknown
required entity is loaded and exposed for inspection, but the document becomes read-only. An
unknown project `format_version` is rejected rather than opened unsafely.

## SQLite schema

The SQLite `application_id` is `0x50535444` (`PSTD`). `user_version` and metadata
`format_version` are equal and range from `1` through `61`, according to the
retained semantics. The baseline application tables below are shared; later
versions add the proof columns and recovery data documented in this file.

| Table | Purpose |
| --- | --- |
| `metadata` | `format_version`, `document_id`, `head_revision`, `saved_revision`, and `logical_digest` |
| `revisions` | Revision identity, event parent/source, action/name, and undo/redo navigation stacks |
| `revision_entities` | Full entity state for each revision, including properties and extension JSON |
| `revision_assets` | Full pinned asset bytes, media type, metadata JSON, and SHA-256 for each revision |
| `named_revisions` | Stable name-to-revision mappings, including retained branches |

All application tables are SQLite `STRICT` tables with primary and foreign keys. A standalone
v1–3 file must have `saved_revision == head_revision`, use SQLite `DELETE` journal mode, and have no
`-journal`, `-wal`, or `-shm` sidecar. Load verifies the exact metadata-key set, `user_version`,
table columns, primary keys, foreign keys, and `STRICT` flags. It rejects missing or additional
schema objects, invalid column types, non-contiguous or impossible history transitions, a
non-bijective named-revision index, duplicate IDs, invalid JSON, broken references, oversized
data, asset hash mismatches, and a logical digest mismatch.

The logical digest is BCrypt SHA-256 over a deterministic JSON manifest of metadata, revision
structure, entities, asset metadata, asset sizes, and asset SHA-256 values. Each asset's SHA-256
binds its bytes to that manifest. This detects corruption and uncoordinated modification. It is
not a signature or message authentication code and does not prove who created a file.

Current safety bounds are 128 bytes for IDs, 64 bytes for entity types, 1 MiB per JSON object,
64 levels and 100,000 values per JSON object, 256 MiB per asset, 4 GiB per project file, 10,000
revisions, 250,000 entity rows, 100,000 asset rows, 64 MiB of encoded JSON, 2,000,000 aggregate
JSON values, and 512 MiB of aggregate decoded asset bytes. Physical size and SQL aggregates are
checked before graph/blob allocation; allocation failures are translated to typed resource-limit
errors. The SQL preflight measures JSON as UTF-8 bytes (`length(CAST(value AS BLOB))`), rather than
SQLite text characters. IDs allow ASCII letters, digits, dash, underscore, dot, and colon. Project
paths must name ordinary files under an existing, non-reparse-point parent directory. Windows
device names and alternate data streams are rejected; ambiguous trailing-dot or trailing-space
names are rejected when creating a destination.

The separate recovery-aware v4 format adds a recovery-record table and preserves
the optional captured saved revision. Its complete schema, digest and opaque
load contract are documented in [project-archive-v4.md](project-archive-v4.md).
Document-only APIs refuse v4 rather than discard its recovery ledger.

Version 5 is required when any retained revision contains an explicit boundary
translation proof, including an undone command or an abandoned branch. It adds
one nullable `TEXT` column, `revisions.boundary_translation_json`, after
`redo_stack_json`. SQL `NULL` means no proof; a JSON `null` value is invalid.
A present proof has exactly `{"version":1,"boundary_id":"...","offset":[x,y]}`.
The version is an integer, the boundary ID obeys the ordinary identifier rules,
and both offsets are finite numbers in metres. Unknown versions, extra keys,
duplicate keys and malformed values reject. Proof JSON participates in the
aggregate byte, value and recovery string budgets.

A v5 file may contain the same `project_recovery_records` table as v4. Its
presence makes the file an archive: recovery-aware APIs preserve its ledger and
optional saved revision, and document-only load or replacement rejects it.
Without that table, v5 has the standalone document saved-revision rules. The
exact expected schema is checked in both cases. Histories without proofs retain
the existing v1-v4 schemas and digest encodings.

The proof is included in logical and document/source digests. On restore the
document recomputes the complete next entity state from the previous revision
and the offset; it rejects unrelated edits, changed assets, forged offsets or
missing proofs even after an attacker recomputes the logical digest. Human
action text grants no authority. Exact undo/redo references retain the original
proof on its command revision rather than copying it onto navigation records.
JSON/assets extraction uses exchange version 2 when proofs occur and emits
`boundary_translation` on the corresponding revision; proof-free extraction
remains exchange version 1.

Version 6 is required when any retained revision contains an explicit boundary
transform proof, including undone commands and abandoned branches. It retains
the v5 translation column and adds nullable `TEXT`
`revisions.boundary_transform_json`. A transform proof has exactly `version`
(integer 1), `boundary_id`, `pivot`, `rotation_radians`, `flip_horizontal`,
`flip_vertical`, and `offset`. Points are finite two-number arrays in metres,
the angle is finite radians, and both flip fields are Booleans. Unknown versions,
duplicate or extra keys, and malformed values reject. SQL NULL is absence;
JSON null is invalid. A revision cannot contain more than one boundary
derivation proof.

Transform proofs participate in the same digests, resource budgets, complete
state reconstruction, and navigation restrictions as translation proofs.
The command preserves identities and local receipt inputs, appends an ordered
schema-3 frame, and transforms attached dimension positions. Unrelated entities
and assets must remain identical. A v6 archive is distinguished by its recovery
table, as in v5; document-only APIs cannot discard that ledger. Histories without
transform proofs keep their earlier minimum format and digest representation.
JSON/assets extraction uses exchange version 3 when a transform proof occurs
and emits `boundary_transform` on its command revision. Translation-only and
proof-free histories retain exchange versions 2 and 1, respectively.

Version 7 adds nullable `revisions.boundary_edit_json`. A present value is a
strict version-1 `move_vertex`, `resize_segment`, `insert_vertex`, or `redefine_boundary` intent, or a version-2 redraw reference intent described below. Vertex moves store the
boundary ID, stable vertex ID, and absolute finite position. Segment resize
intents store the boundary ID, stable segment ID, positive analytical length in
metres, fixed endpoint (`start` or `end`), and the explicit connected-chain
choice. Insertion stores the boundary and target segment IDs, a finite fraction
strictly between zero and one, fresh `new_vertex_id` and `new_segment_id`, and
`new_dimension_id` (an empty string when no automatic second-piece dimension
is requested). The original segment ID remains on the first piece; its end
changes to the inserted vertex, and the new second piece ends at the original
end vertex. The edit retains all existing boundary, segment, and vertex IDs.
Only a validated typed insertion may authorize this endpoint ownership change;
raw entity replacement cannot rebind that ID. New IDs must not collide with
current or retired topology IDs in that boundary; the new dimension ID must
also be unused in retained entity history. Readers predating insertion support
reject its unknown intent rather than silently discarding it.

Redefinition stores ordered identified `replacement_segments`, optional exact
raw `replacement_authoring`, restricted `replacement_properties`, and explicit
`replacement_dimension_ids`. Equal edge counts retain every ordered child ID;
changed counts allocate fresh children and regenerate automatic edge dimensions.
Owner-only area references remain attached. Changed-count redraws with child
references require explicit decisions; missing or incompatible decisions reject
without changing the source. A nonempty reference plan uses strict intent version
2 and adds both `replacement_child_mapping` (an object with `segments` and
`vertices` objects, each mapping old child IDs to new IDs) and
`replacement_removed_reference_ids` (an ordered array of reference
entity IDs). Empty plans retain the exact version-1 encoding and size policy.
Maps connect existing segments to replacement segments and vertices to vertices,
with distinct destinations within each namespace. Equal ID strings in the segment
and vertex namespaces remain separate. A mapping with no entries is encoded as
`{}`; nonempty mappings include both typed groups. Every map entry must be used by a retained reference.
Every retained child reference must be fully mapped, including angle dimensions'
second edge and common vertex. Constraint endpoint roles follow the new vertex's
incidence on the mapped segment; relation, length, anchor, other owners and opaque
binding metadata remain unchanged. Only affected supported manual child
dimensions or endpoint constraints may be explicitly removed. Automatic edge
dimensions regenerate; automatic angle dimensions may be mapped but not removed.
Area dimensions, unrelated entities and unsupported references cannot be removed
through this plan. Canonical constraint checks still reject conflicting locks.
Non-null construction inputs must replay to the exact replacement geometry before
their temporary child IDs are mapped by order. Raw API geometry uses null
construction evidence. Receipt-free drawn replacements acquire verified new
construction evidence; existing receipts and derivation prefixes remain exact.
The proof field has a 1 MiB read budget with aggregate JSON resource accounting;
replacement payload validation reserves encoding overhead within that limit.

A workspace redraw is one `boundary_finish` operation: the typed replacement,
input archival and draft retirement publish together. The archived strict
`desktop_operation` identifies the target. Version 1 contains exactly `version`,
`kind` (`redefine`) and `target_id`; nonempty accepted plans use version 2 and add
the two reference-plan fields above and `replacement_segments_sha256`, the SHA-256
of the exact ordered replacement-segment JSON encoding. This binds fresh IDs to
their reviewed geometry without duplicating the geometry payload. Atomic finish and history restoration require
the entire envelope to match the typed command exactly, including ordered removal
IDs. Editing the draft invalidates previous review choices. Restoration checks source revision,
resolved drawing context, exactly one accepted classified chain, exact new
construction input, canonical classification/category updates and the complete
replayed entity map. One Undo restores prior geometry and retains the redraw
input as a retired recoverable view; Redo restores the exact replacement.

The first direct edit of a receipt-backed boundary moves the exact original
`boundary_authoring` envelope into
`extensions.boundary_geometry_derivation.source_boundary_authoring` and appends
the edit intent to its ordered `operations` array. Subsequent coordinate edits,
insertions, translations, rotations, and reflections append in command order. Load and
document restoration replay the original construction and every operation, then
require an exact match with canonical geometry and command history.
The original construction evidence is therefore preserved without pretending
that it produced the manually edited coordinates. A missing, forged, reordered,
or incompatible edit proof rejects the project. A v7 archive is distinguished
by its recovery table exactly like v5 and v6. JSON/assets extraction uses
exchange version 4 and writes `boundary_geometry_edit` on each corresponding
command revision, including retained undone and abandoned history.

## Save and replacement protocol

`ProjectStore::save` takes an immutable snapshot. If the destination exists,
`expected_destination_sha256` is mandatory and must equal the fingerprint returned by the
preceding load or save. A missing, replaced, or modified destination fails closed.

The save sequence is:

1. Open the parent directory and reject a reparse point. For an existing destination, open the
   complete destination and use its handle-resolved, volume-backed normalized path; this expands
   extended, trailing-dot, and available DOS 8.3 aliases. For a missing destination, append only a
   non-ambiguous final filename to the handle-resolved parent. Apply Windows invariant case folding
   and acquire the corresponding `Global` named mutex. Failure to resolve or access the identity
   fails closed. One expected fingerprint authorizes at most one cooperating save.
2. Reserve a new, unique file with Windows `CREATE_NEW` in the destination directory and retain
   that file-object handle through SQLite writing, flush, validation, and publication upgrades.
3. Create the complete SQLite schema and content in one `BEGIN IMMEDIATE` transaction with
   `synchronous=FULL` and `journal_mode=DELETE`, flush SQLite's page cache, close SQLite, and call
   `FlushFileBuffers` before sealing the staging bytes against further writes.
4. Hold a read handle to the staging file while SQLite runs `integrity_check` and
   `foreign_key_check`, decodes and validates the complete document, and verifies asset and logical
   SHA-256 values. After SQLite closes, `ReOpenFile` acquires `DELETE` access to that same file
   object while denying other writes, rename, and deletion. The publication handle is hashed again
   and must equal the validation digest.
5. Recheck the destination fingerprint while holding its file identity read-only, with deletion
   sharing enabled for replacement. Copy that locked expected destination by handle to a unique
   `.bak.<uuid>` sibling, flush it, and verify its SHA-256. Immediately before publication, open the
   current destination name again, require the same Windows file identity and fingerprint, and keep
   both destination read handles through replacement so content writes remain denied. Keep the
   verified backup read handle with no write/delete sharing through publication and receipt return,
   so its exact bytes cannot be changed, renamed, deleted, or replaced in that interval.
6. Atomically rename the validated staging file object on the same volume with
   `SetFileInformationByHandle(FileRenameInfoEx)`. Replacement uses the Windows replace and POSIX
   flags so the verified destination may remain read-open; new-target saves request neither flag.
7. Return the already verified publication-handle digest, the exact saved revision, and the backup
   path.

Failures before publication run checked cleanup for the exact temporary database, its exact
`-journal`, `-wal`, and `-shm` siblings, and any not-yet-published backup. If Windows prevents
removal, the save error retains the original failure and reports the exact residual paths; it does
not claim cleanup succeeded. When more than one removal fails, the diagnostic lists every exact
path and `StorageError::residual_paths()` exposes the complete structured list while preserving the
original error code and message. A process termination can leave a uniquely named staging file or
backup for later recovery cleanup. The fault-injection stages `after_journal_creation`,
`after_database_write`, `after_validation`, and `before_publish`, plus a validation barrier, exist
for deterministic rollback and lock tests. Coverage includes denial of external writes,
rename/deletion, and path replacement while the publication handle is held; exact published digest
and backup bytes; destination write denial plus rename-and-plant revalidation after backup; stale
external fingerprints; backup write/delete/rename/replacement denial; simultaneous locked backup
and staging-sidecar cleanup residuals; multibyte JSON budgets; and cooperating writers split across
normal, extended, trailing-dot, Unicode, and available DOS 8.3 aliases.

A noncooperating process cannot write destination content while the prepublication read handles are
held, but it can still rename or delete a deletion-shared destination in the small interval after
the final pathname-identity check and before handle-based replacement. Windows does not expose a
generic path-level compare-and-swap replace. The verified prior-file backup is the recovery boundary
for that rename-only OS race. Antivirus or file-indexer handles can also make replacement fail, in
which case the original stays in place and the save reports an error.

## Explicitly pending

The in-memory `Document` is a serialized single writer but does not yet have a durable live working
journal. A WAL/FULL edit journal, recovery after process termination during editing, and compaction
remain future work. The desktop now has a broker-backed edit-session lease that reserves the
normalized project path and, for an existing file, its volume/file identity. A cooperating second
open receives an explicit read-only document. The owner rechecks path identity and content digest
before save and records the new identity after an atomic publication, so an external edit or
replacement becomes a visible read-only/save-as boundary instead of a silent overwrite. This
lease is a cooperating-session guard; hostile writers, restart recovery, and clean-machine
qualification remain production work.

The atomic replacement path has deterministic injected-failure coverage, but it has not been
qualified against real machine power loss, filesystem filter drivers, or disk-full conditions at
every write. Save fails closed unless Windows identifies the destination as a local fixed disk
using NTFS or ReFS; UNC paths, mapped network drives, removable media, and other filesystems are
outside the durability boundary and are rejected before staging.

Deep local Windows folders and Unicode filenames use qualified absolute paths
at native file-operation boundaries and SQLite's locking `win32-longpath` VFS.
Public project paths, source provenance, receipt paths and backup names retain
their ordinary spelling. The same destination identity, expected fingerprint,
reparse-point, staging, backup and publication checks apply. This path handling
does not change the project format or expand the durable-filesystem boundary.

## Survey source corrections

Survey measurement boundaries carry `extensions.survey_source.version = 1`.
`report` contains the currently entered calls and their recomputed results;
`added_closing_segment`, `adjusted_final_endpoint`, and `endpoint_adjustment_m`
record the closure choice. The first call-based correction retains
`original_report` and `original_closure`. Corrections also write `placement`
with `version: 1`, metre `anchor_m: [x,y]`, and
`orientation: "called_north_bearings"`. Other source extensions are preserved.
These optional metadata fields use existing entity/history storage and do not
raise the storage version. Undo and redo restore the geometry and source
metadata together. See [survey contracts](survey-georeferencing-contracts.md)
for input validation and dependent-target rules.

## Boundary-constraint transaction history (v8)

Version 8 adds nullable `revisions.boundary_constraint_changes_json`. A present
value is the strict versioned `apply_boundary_constraint_changes` command
envelope, including expected revision, ordered geometry edits, constraint entity
changes, and message. It belongs only to the originating transaction revision;
undo and redo retain that revision and its proof without copying the proof onto
navigation records. Any retained proof requires v8, including undone history.

The column participates in aggregate JSON byte and value limits, the project
logical digest, document snapshot digests, and authoring source history digests.
Loading decodes the exact command kind and validates its replay against the
parent and resulting entity state. Missing, malformed, or forged proof rejects
even when the file's logical digest has been recomputed. Recovery-aware APIs
preserve the column and recovery ledger together; document-only APIs continue
to reject recovery-bearing files. JSON/assets exchange uses version 5 and emits
`boundary_constraint_changes` on the originating revision.

Older supported files load without this optional proof and retain their existing
minimum storage version when saved. No source file is modified by loading or
migration; a new save containing the command writes both format markers as 8
and the v8 schema. Absent proofs remain omitted from digest manifests, preserving
legacy digest vectors.

Mixed straight-wall/boundary transactions use version 2 of the same command
envelope when wall geometry also changes. Version 2 adds a nonempty ordered
`wall_edits` array. Each entry has exactly `wall_id`, `baseline` and
`length_entry`. `baseline` has finite `start` and `end` coordinate pairs and
`sweep_radians: 0`. `length_entry` is null for a connected wall movement or an
object with `original_expression`, `entered_unit` and `exact_metres`; the latter
has integer `numerator` and `denominator`. Recognized units are `m`, `mm`, `cm`,
`ft` and `in`. Re-parsing the expression must reproduce the entered unit and
exact rational length, which must match the proposed baseline.

Replay reconstructs each existing straight wall from its source entity, changing only
its baseline and recognized length receipt. It retains other wall fields and
hosted-opening records. Duplicate wall edits, curved geometry in these historical proofs,
invalid receipts or hosted openings, and incompatible final relationships
reject the complete transaction. Boundary geometry and its derivation proof
are replayed together with the walls before the final relationship checks.
Version 1 remains unchanged for transactions without wall edits. These nested
command versions continue using the v8 storage column and all its digest and
history admission checks.

## Measured group translation history (v9)

Version 9 adds nullable `revisions.boundary_translations_json`. A present value
is the strict version-1 `translate_boundaries` command envelope. It contains
`expected_revision`, a nonempty ordered `translations` array of `{boundary_id,
offset}`, ordinary `entity_changes`, and `message`. It is retained only on the
originating command revision. Any retained batch requires v9, including a batch
that was later undone. Earlier histories keep their existing minimum version.

The document reconstructs every measured translation from its construction or
geometry-derivation evidence before applying ordinary changes. Stable owners,
vertices, segments, exact measurement entries, and dependent dimension targets
survive; dimension text anchors receive the same offset. Duplicate owners and
ordinary changes overlapping translated owners or their dependent dimensions
are rejected. Ordinary boundary changes pass the usual transition admission;
the message cannot grant an exception. Persistent constraints are checked on
the complete final state, allowing joined owners to move together while a
conflicting partial move refuses without publishing any part of the group.

The proof participates in aggregate storage/recovery budgets and logical and
snapshot digests. Loading replays it against its parent and requires the exact
resulting entities and unchanged assets. Forged or misplaced proof, or missing
proof needed to explain a measured geometry change, rejects the history.
An all-zero translation with ordinary supplemental edits is equivalent to an
ordinary edit; removing that redundant proof does not change its admission.
Undo/redo navigation records do not copy the proof. Exchange
version 6 emits `boundary_translations`; mixed older/newer proof histories retain
the highest required exchange version. Absent batch fields remain omitted from
older digest representations.

## Curved endpoint constraints and wall proofs (v10)

Version 10 raises the minimum reader version without adding SQLite columns.
It uses the v9 schema and the existing `boundary_constraint_changes_json`
column. Both format markers and the logical digest advertise version 10 when
any retained revision contains a curved wall proof or a supported constraint
whose endpoint binding resolves to an actual curved wall baseline or identified
boundary segment. A wall carrying `extensions.curve_input_derivation` also
requires v10, including an imported unconstrained wall with no originating
command in its retained history. This includes already satisfied relations that need no
geometry edit, undone relations, and abandoned history. A straight bound edge
on a boundary containing a different curved edge retains its historical floor.
Opaque unknown constraint versions or relations do not acquire curve semantics.
Generic entities may retain a vendor `curve_input_derivation` extension
collision opaquely without changing their format floor; wall derivation
envelopes are reserved and checked by the curve reconstruction validator.

Curved wall edits carry explicit proof `version: 2` and use version 3 of the
`apply_boundary_constraint_changes` command. Historical straight wall entries
retain their exact unversioned representation, including when accompanied by a
curved entry. The signed sweep is fixed while existing relations constrain
endpoints and chord distance. Wall-only curved commands may contain an empty
`boundary_edits` array. Replay preserves original construction evidence and
validates analytical geometry and hosted openings before publishing the
complete transaction. Historical command versions retain their earlier rules.

Loading recomputes the required floor from all retained proof and entity states
before accepting a file. Downgrading both markers to v9 rejects even with a
recomputed logical digest. Older histories retain their previous minimum
formats, digest encodings, migration behavior, and proof representations.
Recovery-aware APIs preserve v10 documents and their ledgers together;
document-only load or replacement cannot discard a v10 recovery ledger.

JSON/assets extraction uses exchange version 7 for the same retained curved
proofs, bound relations, and wall derivation envelopes. It writes the complete versioned command only on
the originating revision. Later ordinary commands and undo records cannot
lower the exchange version; curve-free histories keep exchange versions 1
through 6 according to their existing proofs.

## Straight wall-only endpoint intent (v11)

Version 11 uses the existing v9 SQLite columns. It protects version 4 of
`apply_boundary_constraint_changes`: an empty `boundary_edits` array, a nonempty
`wall_edits` array containing only historical unversioned straight proofs, and
optional constraint entity changes. Version 4 rejects boundary edits or curved
wall proofs. Versions 1/2/3 retain their earlier decoding and encoding rules.

Endpoint authoring emits this typed command when it changes straight walls
without changing a boundary. Replay reconstructs geometry and measurements
from each proof, then applies the shared analytical topology admission and
independent hard-relation/host validation before accepting the complete state.
Undo/redo and abandoned history retain the originating proof. Deleting its wall
later does not reduce the required reader version.

Both SQLite format markers and the logical digest use v11 whenever any retained
revision contains this capability. Loading independently recomputes that
minimum; changing both markers and recalculating the digest cannot downgrade a
v11 history to v10. Mixed curved and straight-only histories retain the highest
minimum version. Projects without a straight wall-only proof retain their
earlier format requirements. JSON/assets extraction advertises exchange
version 8 and retains each exact command on its originating revision.

These records do not retrofit missing endpoint-edit intent into old generic
`ApplyEntityChanges` history. Explicit construction and object transforms
remain distinct commands with their own validation policy.

## Physical arc-length relationships (v12)

Version 12 retains the existing SQLite columns. A supported version-3 constraint
entity with relation `fixed_arc_length` locks the physical length of a single
curved wall baseline or identified boundary edge. Its exact quantity measures
the analytical arc, not the chord. Bindings identify the same owner and segment,
with opposite endpoint roles and verified stable boundary vertex identities.
The signed sweep remains fixed while endpoint coordinates are solved.

Any retained revision containing this relation requires project format 12,
including undone commands and relations or owners deleted later. A subsequent
straight wall-only proof cannot lower that requirement. Both SQLite markers and
the logical digest carry version 12; loading recomputes the minimum independently
and rejects a downgraded file even if its digest was recalculated. Histories
without this relation retain their previous minimum formats.

JSON/assets extraction advertises exchange version 9 for the same retained
semantics. Earlier constraint entity versions 1 and 2 retain their original
encodings. They do not interpret the new relation spelling as endpoint distance;
unrecognized geometric constraints retain their opaque payload and make the
document read-only.

## Direct physical curve-length inputs (v13)

Version 13 retains the existing SQLite columns. A wall edit proof with
`version: 3` requires a positive exact `length_entry` matching the analytical
arc length, and preserves the source's signed sweep. It uses version 5 of
`apply_boundary_constraint_changes`, which may also contain historical straight
proofs, version-2 curved endpoint proofs and boundary edits. An older command
envelope cannot interpret a version-3 proof; version 5 without that proof rejects.
Historical command and proof versions retain their earlier encodings and rules.

The resulting wall records `extensions.constraint_authoring.version: 1` and
`last_length_entry.version: 2`. Its five fields are `version`,
`original_expression`, `entered_unit`, `exact_metres`, and `baseline`. The exact
rational measures physical arc length; the recorded baseline includes the
signed sweep and must exactly match the current wall. Restore independently
checks recognized receipts even when imported without originating history.
Rigid moves and rotations rebase the receipt; they preserve physical length
and signed sweep. Reflection rebases the receipt's signed sweep while preserving
its magnitude and physical length. Existing measured construction provenance
has separate validation: the shared rigid-transform helper now preserves fresh
and existing measured inputs through the version-2 archive described below.
Subsequent typed
endpoint deformation clears a stale known
receipt unless another explicit length was entered. Unknown nested metadata is
preserved on explicit resize and cannot be silently discarded. Future optional
receipt versions remain opaque on open and cannot be edited by this reader.

Any retained version-3 wall proof or known physical input receipt requires
project format 13, including abandoned, undone and deleted history. The loader
recomputes that minimum and refuses a downgrade despite a recomputed digest.
Generic vendor entities do not acquire this wall-specific extension semantics.
Recovery-aware APIs retain the document and ledger together. JSON/assets
extraction advertises exchange version 10 and preserves exact proof/receipt
payloads; extraction does not constitute a JSON project importer.

## Rigid curve construction transforms (v14)

A measured curved wall's first nonidentity rigid transform retains its exact
original `curve_input` and baseline, including unknown nested metadata, in
`extensions.curve_input_derivation.version: 2`. The archive has four fields:
`version`, `source_input`, `source_baseline`, and a nonempty `operations` array.
Historical version-1 archives keep their encoding; adding a rigid transform
upgrades their version and retains every previous operation verbatim.

A rigid operation has exactly three fields: `kind: "rigid_transform"`,
`transform`, and `baseline`. The transform has six fields: `version: 1`,
`pivot: [x,y]`, finite `rotation_radians`, boolean `flip_horizontal` and
`flip_vertical`, and `offset: [x,y]`. Replay rotates about the pivot, reflects
about the pivot, then translates. An odd number of reflections reverses signed
sweep; physical length and sweep magnitude remain unchanged. The recorded
baseline must match that independent reconstruction. Original measurement
expressions remain archived even when reflection changes the active input to a
canonical angle construction. No identity transform adds an archive operation.

Version 2 requires at least one rigid operation and also supports the earlier
fixed-sweep endpoint and explicit reconstruction operations. Admission compares
an appended rigid operation against the exact source entity; source input,
baseline, and prior operations cannot be rewritten or dropped. The existing
typed endpoint proof rules are unchanged.

Every retained or imported wall with a known version-2 archive requires project
format 14, including undone and deleted history. Recovery-aware APIs retain the
same floor. Older format markers reject even with a recomputed logical digest.
JSON/assets extraction advertises exchange version 11 and retains the complete
archive; it does not supply a JSON project importer. Measured-curve scaling
remains unsupported and rejects without changing the document.

## Fixed-chord boundary curvature edits (v15)

`BoundaryGeometryEdit.kind: "reconstruct_arc"` keeps both endpoints and all
segment/vertex identities. Its version-1 JSON envelope contains exactly
`version`, `kind`, `boundary_id`, `segment_id`, and `construction`.
The construction is a strict receipt of kind `arc_chord_angle`,
`arc_chord_height`, or `arc_chord_length`; its segment ID, start, and chord end
must match the selected edge exactly. Angle and height are signed; arc length
uses the receipt's clockwise flag. The receipt kernel validates the retained
original and normalized expressions and reconstructs analytical geometry.
Degenerate curves and invalid closed topology are refused.

The source `boundary_authoring` receipt is archived unchanged in
`extensions.boundary_geometry_derivation`, followed by a replayable
`geometry_edit` operation carrying the complete intent. Existing derivation
operations remain unchanged. Known constraints and dependent dimensions are
validated by the normal document transaction; automatic length dimensions
follow the new arc and winding, while manual placements remain unchanged.

The new intent in any retained revision, or in an imported identified boundary's
derivation, requires format 15. Undoing or deleting the current curve does not
lower that floor. Older format markers reject the retained semantics. Files
without this intent retain their previous required format and serialization.
JSON/assets extraction advertises exchange version 13, including recovery
archives carrying this intent. Extraction is not a JSON project importer.

## Reviewed exterior wall-source replacement (v16)

An explicit source repair is a `redefine_boundary` intent encoded as strict
`BoundaryGeometryEdit` version 3. In addition to the existing redefinition fields,
it requires nonempty `replacement_wall_source_ids` and both reference-plan fields
(`replacement_child_mapping` and `replacement_removed_reference_ids`, possibly
empty). The IDs must identify 3 to 2048 distinct walls and count toward the
persisted proof budget. Other edit kinds cannot carry source replacements. The
existing version-1 and version-2 edit representations are unchanged.

The core independently derives the analytical exterior from those walls. The
replacement outline must match that exterior, allowing cyclic ordering or
reversed direction; a caller cannot provide arbitrary source JSON. The owner and
sources must resolve to the same property/building/floor/layer, consistent eligible
phase and effective elevation plane. Retained deductions must fit the replacement.
Geometry and `wall_measurement_source` change in one transaction; facts, factors,
name, styling, custom attributes and deduction links are preserved. Ordinary
Refresh remains tied to the originally recorded sources.

An authored owner retains the existing version-1 geometry derivation and its
original construction record. An identified owner without a construction record
uses strict `boundary_geometry_derivation` version 2 with exactly `version`,
`source_boundary` and `operations`. `source_boundary` contains only the original
`boundary_model_version` and identified `segments`. No construction receipt is
invented. Historical replay uses this archived geometry and its typed operations,
without rederiving from today's walls. Later wall deletion or editing can make
source freshness false while the project and its history remain readable. The
final source IDs must agree with the latest retained source-replacement intent.

Supported identified measurement owners cannot acquire, change or remove their
wall-source metadata through a raw surviving-entity edit. Generic and legacy
vendor metadata remain separate. Original source v1 does not archive elevation;
when no original walls survive and the owner has no explicit elevation, the
original plane is unavailable. Replacement walls still must share one coherent
effective plane and the resolved original hierarchy and phase.

### Fresh-topology redefinition (v17)

Redefinition intent version 4 adds `fresh_topology: true` and requires both
reference-plan fields plus `replacement_wall_source_ids`; the source ID array may
be empty for a generic redraw. Earlier intent versions retain their existing
wire shapes and format floors. A v4 intent is valid only for boundary
redefinition. It assigns fresh segment and vertex IDs to the replacement
topology, disjoint from every old child ID, rather than guessing correspondence
by order. For changed geometry with equal edge count, the desktop opens the
reference planner so supported manual dimensions and constraints can be mapped
or removed explicitly; automatic dimensions are regenerated. Different edge
counts continue to use explicit reference mapping/removal decisions and regenerate
automatic length dimensions.

This intent anywhere in retained, undone or deleted history, or in an imported
derivation, requires native format 17. Under-versioned archives reject even when
their digests are recomputed. JSON/assets extraction advertises exchange version
15. Files without these new semantics retain their previous required format and
representation; extraction remains separate from project import.

### Measured group rigid transforms (v18)

`TransformBoundaries` retains one shared numerical planar transform and a list
of distinct measured owner IDs. Its version-one command envelope has exactly
`version`, `kind: "transform_boundaries"`, `expected_revision`, `message`,
`transformations` and `entity_changes`. Each transformation uses the existing
single-boundary transform representation. Supplemental changes preserve existing
identities and cannot substitute raw boundary or bound-dimension geometry.

The complete candidate reconstructs all measured owners, dimensions, source walls,
hosted openings and internal relationships together before final admission. Plain
identified boundaries retain an exact topology origin instead of invented input
receipts. Source exterior normalization preserves child identities and is admitted
only with unique machine-precision analytical correspondence and typed replayable
evidence. Incompatible external relationships and invalidated unchanged consumers
are refused; they are not silently dropped or refreshed.

A present group proof uses `revisions.boundary_transforms_json` in SQLite and
`boundary_transforms` in revision JSON/digest data. Absent proofs are omitted from
logical representations to preserve previous digest values. Retained group history
requires native format 18 and extraction version 16, including after Undo or
deletion. Recomputed digests do not authorize lowering those format markers.

New exterior derivations use stable tangent intersection arithmetic. Retained
replacement commands may instead validate against the complete exact original
version-one offset result, including its original gap allowance. This historical
route is separate from live replacement admission and never rounds or rewrites
stored geometry, command bytes or digests. Exact original outlines can also
remain current against their unchanged sources. Old records retain this
compatibility when a later command raises the archive to format 18.

### Coordinated physical wall and exterior updates (v19)

Constraint-command envelope version 6 retains the preceding geometry and
constraint fields, plus `physical_entity_changes` and `exterior_source_edits`.
Physical changes are admitted under their ordinary wall-edit rules; they do not
inherit typed endpoint-edit authority. Exterior edits are explicit typed boundary
redefinitions applied after the physical wall candidate has been reconstructed.
The envelope version remains part of command identity even when testing omission
or tampering; removing a required exterior update cannot downgrade its meaning.

Every automatic exterior update is recomputed from the original physical walls
and measured owner. A unique cyclic or reversed analytical correspondence binds
existing edge and corner identities to their recorded source walls. Proposed
geometry must retain those wall identities and their cyclic adjacency. Equal
edge counts are insufficient. The owner retains its metadata, appraisal facts,
appearance and deduction references; dimensions resolve against the retained
child identities. Final constraints and deduction containment remain mandatory.

Consumer discovery uses the complete original document, including unselected
owners. Initially current supported owners are coordinated together. Previously
stale sources, generic imported boundaries and explicit source deletion retain
their repair contract and cannot produce qualified totals merely from this proof.
Changing source topology still requires reviewed replacement and reference mapping.

The proof occupies the existing `boundary_constraint_changes_json` revision
column and corresponding logical/extraction data; no new SQLite column is
required. Retained v6 history requires native format 19 and extraction version 17,
including after Undo or deletion. Existing v1-v5 command envelopes retain their
original replay order and digest bytes.

### Mixed ordinary objects and exterior updates (v20)

Constraint-command envelope version 7 additionally records
`supplemental_entity_changes` and `supplemental_asset_changes`. These lanes retain
ordinary object and asset intent alongside the version-six physical wall and
exterior update proof. Version seven can also retain an ordinary physical wall
edit with additional author-supplied wall metadata. Ordinary construction
provenance and source context remain subject to their existing checks; this does
not grant permission to replace a retained construction record. Version-six
physical metadata restrictions remain unchanged. The version marker remains retained even if supplemental
vectors are emptied; removing intent cannot reinterpret a recorded operation as
an older command. Version-six commands keep their original representation.

The original ordinary command must be admitted before source completion. A
supplement cannot overlap another lane or inject raw measured-owner or bound
dimension geometry. Ordinary provenance, constraints, references and asset
validation remain mandatory. The complete candidate carries exact entity and
asset changes through preview, atomic Apply, Undo/Redo and retained history.
Exterior reconstruction still covers every eligible current consumer and requires
its existing source-wall lineage; unrelated objects confer no additional geometry
authority.

Retained v7 history requires native format 20 and extraction version 18, including
after Undo or deletion. Its proof uses the existing revision JSON column. Lowering
format markers remains invalid even when digests are recomputed. Earlier formats
and command digests retain their prior semantics.

Legacy envelope 7 keeps its original one-MiB encoded proof ceiling and inline
hexadecimal asset bytes. New changed-asset source completion uses envelope 9
and native format 26, described above, to avoid duplicating those bytes in JSON.

### ANSI-oriented appraisal evidence (v21)

The opt-in `appraisal_policy.policy_kind: "ansi_z765_2021"` has policy version
one and calculation profile `vertex-ansi-z765-2021-v1`. Existing declared
residential and light-commercial profiles retain their previous behavior.
This is a versioned Vertex rule implementation, not ANSI approval or a complete
UAD reporting contract. Rule evidence and unresolved normative interpretations
are part of the measurement summary.

The property policy's `ansi` object records `interior_inspected` and
`direct_measurement` booleans, `acquisition_increment` (`inch` or `tenth_foot`),
and `limitations_statement`. A floor's `appraisal_facts.ansi` records
`any_part_below_grade`; it must agree with the declared whole-level `grade`.
Missing observations remain undeclared; they are not assumed true.

An area's `appraisal_facts.ansi` records `year_round_suitable`,
`finish_matches_dwelling`, `dwelling_identity` (`primary`, `attached_adu`,
`detached_adu`, `detached_other`) and `ceiling`. Ceiling `kind` is `flat`,
`sloped` or `stairs`. Flat evidence has `minimum_height_m`. Sloped evidence has
`at_least_7ft_area_m2`, `room_floor_area_m2`, `room_boundary_id`,
`source_geometry_sha256`, and `below_5ft_deduction_ids`. Heights and areas are
stored in metres and square metres, independently of workspace display units.
Stairs have `stair_from_floor_id` and must have `stair_footprint` boundary role.
Access additionally supports `through_unfinished` for the new profile.

Sloped evidence binds the complete room boundary and deduction geometry using
the versioned deterministic ceiling geometry digest. Referenced low-height
areas must be real contained geometric deductions in `deduction_ids`; scalar
area declarations never replace exclusion geometry. Geometry changes invalidate
the evidence rather than silently preserving measured proportions. ANSI room
partitions can have nested deductions; cyclic graphs, incompatible floor context
and overlaps that would double-count area remain errors. A parent removes the
whole child footprint and the child contributes only its own net category area.

Primary, ADU and detached-other categories are separate. The canonical report
rounds aggregate square feet once to whole square feet; boundary dimensions use
tenths of a foot. Display rounding does not change stored geometry. The sloped
V1 denominator uses gross room geometry and remains explicitly provisional.
V2 uses the countable finished-room rule described above. Final publisher
standard validation remains pending for both versions.

These semantics use existing entity JSON and require native format 21 and
extraction version 19 even when retained only in undone or deleted history.
Earlier readers must refuse them rather than edit away the observations. A
recomputed digest does not permit lowering the reader floor. Unrelated vendor
properties outside the property/floor/boundary appraisal namespaces remain
opaque and do not raise this floor. Older documents retain their existing
minimum format and digest representations. Extraction is a documented data
export and does not itself provide project import.

### Coordinated exterior-corner edits (v22)

Constraint-command envelope version 8 retains an `exterior_corner_move` intent.
Its strict version-1 object contains `version`, `boundary_id`, `vertex_id`,
`position` (finite X/Y in model metres), and `move_connected_objects`.
The boundary and vertex IDs identify a current physical-wall-derived measured
exterior. A stale or ambiguous source cannot authorize this operation.

Replay independently reconstructs physical walls from the requested analytical
outline, wall thicknesses and retained source lineage, then regenerates affected
measured owners. It validates the desired outline against the forward derivation
within the existing 1e-7-metre geometry tolerance. Stored boundary bytes remain
the forward-derived authority; the requested coordinate is never substituted
into derived geometry merely to make a source look current. Stable measured
edge/vertex IDs remain retained.

This intent supplies narrowly scoped authority for physical arc reconstruction.
Original measured curve inputs and previous derivation operations remain
archived. Existing fixed-sweep wall proof versions keep their original rules.
Unproved ordinary physical or asset supplements cannot use version 8's corner
authority. Saved constraints, hosted-opening fit, physical contact topology,
deductions and all previously-current source consumers require final validation.
Preview and Apply use the same command; the complete edit occupies one revision.

Endpoint joints and fractional T stations are reconstructed as temporary solver
relations from the original physical contacts. They do not introduce a persisted
constraint kind. The version-8 topology check uses resolved elevations and active
physical phases, preserving unavailable wall payloads. Earlier envelopes keep
their historical topology policy.

Every retained version-8 command requires native format 22, including undone or
deleted history. JSON/assets extraction advertises exchange version 20.
Older document semantics retain their existing format floors and command bytes.

## Measured-stroke constraints and saved dimensions (native 35–36)

Native 35 protects retained measured replay/schema 5, command envelope 11 and
constraint bindings to measured strokes. These forms retain original entered
quantities, stable edge and vertex IDs, simultaneous vertex corrections and
explicit rigid transformations. The command completes eligible source-area
consequences and source-owned annotation offsets before final validation.

Native 36 additionally admits existing dimension models 1 and 2 whose
`target.entity_id` names a `measurement_linework` owner. Length targets use its
stable segment ID and resolve physical analytical length. Angle targets name
two distinct segments and their actual shared stable vertex, resolving outgoing
endpoint tangents. Open, terminal and revisited vertices remain legitimate;
closed strokes still cannot supply an area dimension. Unsupported owner models
remain opaque/read-only rather than acquiring inferred area semantics.

Ordinary vertex or length edits retain placed text coordinates and presentation.
Explicit measured rigid transforms reconstruct attached text coordinates once
with the complete pivot, reflection, rotation and translation. Mixed typed
boundary movement verifies the supplemental stroke against the same source
transform before reconstructing dimension positions; overlapping dimension
replacements refuse. Ordinary desktop mixed-object movement proves each stroke
payload by replay, places its dimensions from source, then validates the whole
ordinary transaction, including selected constraints and anchors.

Both native markers declare 36 whenever any retained revision has a dimension
targeting a measured stroke, including a genuine v1 stroke, entity-only import,
undone creation or deleted dimension. Logical JSON/assets extraction advertises
34 for this reader floor (native 35 maps to extraction 33). No SQLite table or
dimension schema change is required. Lowering both markers cannot make retained
new semantics readable by an older format. Original files remain preserved
through the normal save/migration workflow.

## Physical wall splits and full-span dimensions (native37)

Command envelope12 is a strict `apply_boundary_constraint_changes` envelope
containing only version, kind, expected_revision, message and wall_split. Its
version-one intent names the original wall, fresh second wall and coincident
seam relation, a strict interior fraction, and fresh analytical identities for
each affected source-current measured owner. It admits no ordinary entity or
asset mutation lanes. Replay independently reconstructs every consequence from
the retained source before comparison with saved state. Historical reuse of
entity or analytical identities rejects.

Each wall's version-one `extensions.wall_split_archive` retains directed source
and partition baselines, source identity, fraction, piece role and original exact
length entry. Its historical geometry is independently validated and retained
unchanged by subsequent ordinary edits. Signed arcs retain their construction
archive through the existing analytical reconstruction path. Unsupported future
archives remain opaque/read-only.

Fixed physical arc-length constraints use relation version4 with ordered pairs
of opposite endpoints for contiguous directed curved pieces. One equation sums
their physical lengths at their saved signed sweeps against the original exact
quantity. It does not impose independent piece locks or new equal-radius rules.
Dimension version3 stores `target.segment_ids` instead of `target.segment_id`
for an ordered contiguous chain. Its value is the sum of analytical member
lengths, retaining text placement, presentation and opaque metadata.

Reader37 applies to split commands, split archives, version4 arc-total relations
and version3 dimensions anywhere in retained history, including entity-only
imports, undone creation and deleted objects. Logical extraction advertises
exchange35. Both native markers and the package validator agree; existing older
histories retain their previous minimum floor. No SQLite table change is needed.
