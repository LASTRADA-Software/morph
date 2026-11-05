.pragma library

// The rule every container that lays children out along an axis shares: children that stretch divide the room left
// by the others and the gaps by weight, starting from zero, and the container asks its parent for what lets each of
// them hold its natural length.

// Shares @p room among @p members, which hold a `weight`, a `minimum` and a `maximum`, by weight from zero. A share
// outside its bounds is held at the bound and the rest is shared again among the others (the bounds of a flex
// layout): each pass shares the room among the open members and clamps; when the clamps added length in total the
// members raised to their minimum are held, when they removed length the members cut to their maximum are, and when
// they changed nothing every member is. A pass holds at least one member, so there are at most as many passes as
// members. Returns the lengths, aligned with @p members.
function shareByWeight(members, room) {
    const lengths = members.map(() => 0)
    let open = members.map((member, i) => i)
    let spare = room
    while (open.length > 0) {
        let weights = 0
        for (const i of open)
            weights += members[i].weight
        const clamped = {}
        let added = 0
        for (const i of open) {
            const raw = Math.max(0, spare) * members[i].weight / weights
            clamped[i] = Math.min(Math.max(raw, members[i].minimum), members[i].maximum)
            added += clamped[i] - raw
        }
        const frozen = []
        for (const i of open) {
            const raw = Math.max(0, spare) * members[i].weight / weights
            if (Math.abs(added) < 1e-6 || (added > 0 ? clamped[i] > raw : clamped[i] < raw))
                frozen.push(i)
        }
        for (const i of frozen) {
            lengths[i] = clamped[i]
            spare -= clamped[i]
        }
        open = open.filter(i => !frozen.includes(i))
    }
    return lengths
}

// What the stretching children together ask for: `W * max(n / w)`, where `n` is a child's natural length within its
// bounds and `w` its weight (`members` hold `natural` and `weight`), and `W` the sum of the weights, so that each
// share is at least its child's natural. With one member that is its natural.
function stretchNatural(members) {
    let weights = 0
    let ratio = 0
    for (const member of members) {
        weights += member.weight
        ratio = Math.max(ratio, member.natural / member.weight)
    }
    return weights * ratio
}
