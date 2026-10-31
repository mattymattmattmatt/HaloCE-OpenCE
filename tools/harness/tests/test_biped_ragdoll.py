"""A body's ragdoll in a multiplayer game (port/linux/game/biped_ragdoll.c): it falls onto the ground in one piece and
comes to rest, its nodes turn with its bones, and a blow turns it."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure

CASES = ['falls_and_rests', 'nodes_follow_bones', 'push_turns', 'thrown', 'knees_bend_one_way', 'hips_swing_so_far',
         'torso_heavier', 'limbs_keep_out', 'halo2_falls']
CONTROLS = [
    ('\tragdoll_solve(ragdoll);\n', '\n', 'falls_and_rests'),
    ('rotation_between(&before, &after, turn);', 'rotation_identity(turn);', 'nodes_follow_bones'),
    ('weights[index] = 1.f / (1.f + distance * distance);', 'weights[index] = 1.f;', 'push_turns'),
    ('ragdoll_clamp_speed(velocity);\n\t\tstarts', 'starts', 'thrown'),
    ('if (across < 0.f)\n\t\t\tacross = 0.f;', '', 'knees_bend_one_way'),
    ('twist = ragdoll_clamp(twist, -limit, limit);', '', 'knees_bend_one_way'),
    ('if (cosine >= joint->limit)\n\t\t\treturn;', 'return;', 'hips_swing_so_far'),
    ('weight_a = ragdoll->inverse_masses[constraint->a];', 'weight_a = 1.f;', 'torso_heavier'),
    ('if (constraint->type == _ragdoll_constraint_minimum && distance >= constraint->length)\n\t\t\t\tcontinue;',
     'if (constraint->type == _ragdoll_constraint_minimum)\n\t\t\t\tcontinue;', 'limbs_keep_out'),
    ('velocity->k -= (velocity->k - ragdoll->velocities[parent].k) * ragdoll->joint_friction;', '', 'limbs_keep_out'),
]
FUNCTIONS = ['ragdoll_clamp', 'ragdoll_clamp_speed', 'ragdoll_move', 'rotation_identity', 'rotation_transform',
             'rotation_between', 'basis_from_directions', 'rotation_between_bases', 'ragdoll_add_constraint',
             'ragdoll_add_bend', 'ragdoll_push', 'ragdoll_frame', 'rotation_untransform', 'ragdoll_frame_node',
             'ragdoll_add_hinge', 'ragdoll_add_cone', 'ragdoll_hold_joint', 'ragdoll_solve', 'ragdoll_substep',
             'ragdoll_pose']


def generated(fault=None):
    source = read('port/linux/game/biped_ragdoll.c')
    code = '\n'.join(
        [enum_with(source, 'MAXIMUM_RAGDOLLS'), enum_with(source, '_ragdoll_constraint_minimum'),
         enum_with(source, '_ragdoll_joint_cone'), enum_with(source, '_ragdoll_style_halo2')] +
        re.findall(r'^#define RAGDOLL_\w+ [^\n]*', source, re.M) +
        [structure(source, name) for name in ('ragdoll_constraint', 'ragdoll_rotation', 'ragdoll_joint', 'ragdoll')] +
        [function(source, name) for name in FUNCTIONS])
    if fault:
        code = mutated(code, *fault)
    return (('under_test.inc', code),)


@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('biped_ragdoll', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('before,after,case', CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build('biped_ragdoll', generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_ragdoll_where():
    # The setting chooses the ragdoll in a campaign and in multiplayer. A cinematic, and a body marked as an
    # actor in one, keep the scripted death.
    source = read('port/linux/game/biped_ragdoll.c')
    style = function(source, 'ragdoll_style')
    should = function(source, 'ragdoll_should_start')
    assert 'config_string("game.ragdoll_players")' in style and 'config_string("game.ragdoll_ai")' in style
    assert 'player_index' in style and 'game_engine_running()' not in style
    assert 'ragdoll_style(biped_index) != _ragdoll_style_none' in should
    assert 'cinematic_in_progress()' in should and '_object_movie_star_bit' in should


def test_host_word_skips_ragdolls():
    # A client takes no correction of a body its own ragdoll moves.
    states = function(read('port/linux/game/network_objects.c'), 'network_objects_handle_states')
    skip = states.index('biped_ragdoll_active(state->object_index)')
    assert skip < states.index('distributed_body_set_at_rest(state->object_index, at_rest);')


def test_host_only_draws():
    # Hosting a network game, a ragdoll is only drawn: the body moves and poses as the stock game has it (what the
    # host sends its clients and what its shots hit), and the renderer draws the ragdoll's nodes.
    source = read('port/linux/game/biped_ragdoll.c')
    assert 'ragdoll->drawn_only = game_connection() == _game_connection_network_server;' in function(source, 'ragdoll_start')
    update = function(source, 'biped_ragdoll_update')
    assert update.index('if (ragdoll->drawn_only)\n\t\treturn FALSE;') < update.index('ragdoll_move_body(')
    assert 'return !ragdoll->drawn_only;' in function(source, 'biped_ragdoll_accelerate')
    for name in ('ragdoll_sleep', 'ragdoll_wake'):
        body = function(source, name)
        assert body.index('drawn_only') < body.index('SET_FLAG(biped->')
    drawn = function(read('port/linux/game/render_interpolation.c'), 'render_interpolation_object_node_matrices')
    assert 'biped_ragdoll_drawn_nodes(object_index)' in drawn
