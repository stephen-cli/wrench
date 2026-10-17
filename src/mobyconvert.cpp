/*
	wrench - A set of modding tools for the Ratchet & Clank PS2 games.
	Copyright (C) 2019-2023 chaoticgd

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

// Executive Edition fork: convert a moby class core from one game's format to
// another's, through the engine's own reader and writer (MOBY::read_class and
// MOBY::write_class share one in-memory form for all four games).
//
//   mobyconvert <input core.bin> <input game> <output core.bin> <output game> [<reference core.bin>]
//
// Games: rac, gc, uya, dl. For R&C1 output, header bytes 0xa and 0xb (which
// only R&C1 uses) are copied from the reference class, e.g. the R&C1 class
// the converted one stands in for.

#include <core/filesystem.h>
#include <engine/moby_low.h>

static Game parse_game(const char* name)
{
	if (strcmp(name, "rac") == 0) return Game::RAC;
	if (strcmp(name, "gc") == 0) return Game::GC;
	if (strcmp(name, "uya") == 0) return Game::UYA;
	if (strcmp(name, "dl") == 0) return Game::DL;
	fprintf(stderr, "error: unknown game '%s' (rac, gc, uya, dl)\n", name);
	exit(1);
}

static void describe(const char* what, const std::vector<u8>& bytes)
{
	if (bytes.size() < sizeof(MOBY::MobyClassHeader)) {
		return;
	}
	const u8* h = bytes.data();
	f32 scale;
	memcpy(&scale, h + 0x24, 4);
	printf("%s: %zu bytes, high %d low %d metal %d (from %d), joints %d, bytes a/b %02x %02x, sequences %d, sounds %d, scale %f\n",
		what, bytes.size(), h[4], h[5], h[6], h[7], h[8], h[0xa], h[0xb], h[0xc], h[0xd], scale);
}

int main(int argc, const char** argv)
{
	if (argc != 5 && argc != 6) {
		fprintf(stderr, "usage: %s <input core.bin> <input game> <output core.bin> <output game> [<reference core.bin>]\n",
			argc > 0 ? argv[0] : "mobyconvert");
		return 1;
	}
	std::vector<u8> input = read_file(fs::path(argv[1]));
	Game from = parse_game(argv[2]);
	Game to = parse_game(argv[4]);
	describe("input", input);

	MOBY::MobyClassData moby = MOBY::read_class(Buffer(input), from);

	if (to == Game::RAC && argc == 6) {
		std::vector<u8> reference = read_file(fs::path(argv[5]));
		describe("reference", reference);
		moby.rac1_byte_a = reference.at(0xa);
		moby.rac1_byte_b = reference.at(0xb);
	}

	std::vector<u8> output;
	MOBY::write_class(OutBuffer(output), moby, to);
	write_file(fs::path(argv[3]), output);
	describe("output", output);

	// Read it back as the target game to make sure the result parses.
	MOBY::MobyClassData check = MOBY::read_class(Buffer(output), to);
	printf("read back: %zu high, %zu low packets, %zu sequences, %zu joints\n",
		check.mesh.high_lod.size(), check.mesh.low_lod.size(),
		check.animation.sequences.size(), (size_t) check.animation.joint_count);
	return 0;
}
