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
//               [--rebind <from joint> <to joint> <z top> <z bottom>] [--mesh <glb>]
//   mobyconvert --packets <core.bin> <game>
//
// Games: rac, gc, uya, dl. For R&C1 output, header bytes 0xa and 0xb (which
// only R&C1 uses) are copied from the reference class, e.g. the R&C1 class
// the converted one stands in for.
//
// --rebind moves the skin of the part of the mesh below a height from one
// joint to another (heights in the class's vertex units, z up): vertices on
// <from> alone follow <to> below the midpoint of <z top> and <z bottom>, and
// two-way blends of the two joints are re-weighted linearly from all <from>
// at <z top> to all <to> at <z bottom>. Vertices keep their order (it is
// their VU1 slot); each packet gets one more matrix transfer, <to> into a
// VU0 address no vertex uses.
//
// --mesh rebuilds the high and the low LOD of an unskinned class from the mesh
// of a glb's node "moby", written the way wrench's unpack writes it: one
// triangle strip per primitive, and materials named material_<i> for the
// class's texture i. The packets are made by the engine's own builder
// (MOBY::split_packets and MOBY::build_packets); the rest of the class is
// kept. --packets prints each packet's tables, for comparing a rebuilt class
// with the one it replaces.

#include <core/filesystem.h>
#include <core/gltf.h>
#include <core/material.h>
#include <engine/moby_high.h>
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

// mobyconvert --skin <core.bin> <game>: print each high LOD packet's matrix
// transfers and vertices (blend kind, scratchpad joint, VU0 addresses,
// weights, position) for comparing how two classes are skinned.
static int dump_skin(const char* path, Game game)
{
	std::vector<u8> input = read_file(fs::path(path));
	MOBY::MobyClassData moby = MOBY::read_class(Buffer(input), game);
	for (size_t p = 0; p < moby.mesh.high_lod.size(); p++) {
		const MOBY::VertexTable& table = moby.mesh.high_lod[p].vertex_table;
		printf("packet %zu transfers", p);
		for (const MOBY::MobyMatrixTransfer& transfer : table.preloop_matrix_transfers) {
			printf(" %d>%02x", transfer.spr_joint_index, transfer.vu0_dest_addr);
		}
		printf(" two %d three %d main %d\n", table.two_way_blend_vertex_count, table.three_way_blend_vertex_count, table.main_vertex_count);
		for (size_t i = 0; i < table.vertices.size(); i++) {
			const MOBY::MobyVertex& v = table.vertices[i];
			const u8* b = (const u8*) &v;
			const char* kind = i < (size_t) table.two_way_blend_vertex_count ? "two"
				: i < (size_t) (table.two_way_blend_vertex_count + table.three_way_blend_vertex_count) ? "three" : "main";
			printf("v %s hi %d b %02x %02x %02x %02x %02x %02x pos %d %d %d idx %d\n", kind,
				v.v.i.low_halfword >> 9, b[2], b[3], b[4], b[5], b[6], b[7], v.v.x, v.v.y, v.v.z, v.v.i.low_halfword & 0x1ff);
		}
	}
	return 0;
}

static void dump_lod(const char* lod, const std::vector<MOBY::MobyPacket>& packets)
{
	for (size_t p = 0; p < packets.size(); p++) {
		const MOBY::MobyPacket& packet = packets[p];
		const MOBY::VertexTable& table = packet.vertex_table;
		size_t neutral = 0;
		for (u8 b : table.unknown_e_data) {
			neutral += b == 0x80;
		}
		printf("%s %zu: transfers %zu two %d three %d main %d dup %zu verts %zu colours %zu/%zu neutral, indices %zu secret %zu sts %zu header %02x\n",
			lod, p, table.preloop_matrix_transfers.size(), table.two_way_blend_vertex_count, table.three_way_blend_vertex_count,
			table.main_vertex_count, table.duplicate_vertices.size(), table.vertices.size(), neutral, table.unknown_e_data.size(),
			packet.vif.indices.size(), packet.vif.secret_indices.size(), packet.sts.size(), packet.vif.index_header_first_byte);
		for (const MOBY::MobyMatrixTransfer& transfer : table.preloop_matrix_transfers) {
			printf("  transfer %d>%02x\n", transfer.spr_joint_index, transfer.vu0_dest_addr);
		}
		for (const MOBY::MobyTexturePrimitive& t : packet.vif.textures) {
			printf("  texture tex1 %08x %08x %02x %04x clamp %08x %08x %02x %04x tex0 %08x %08x %02x %04x miptbp1 %08x %08x %02x %04x\n",
				t.d1_tex1_1.data_lo, t.d1_tex1_1.data_hi, t.d1_tex1_1.address, t.d1_tex1_1.pad_a,
				t.d2_clamp_1.data_lo, t.d2_clamp_1.data_hi, t.d2_clamp_1.address, t.d2_clamp_1.pad_a,
				t.d3_tex0_1.data_lo, t.d3_tex0_1.data_hi, t.d3_tex0_1.address, t.d3_tex0_1.pad_a,
				t.d4_miptbp1_1.data_lo, t.d4_miptbp1_1.data_hi, t.d4_miptbp1_1.address, t.d4_miptbp1_1.pad_a);
		}
		if (!table.vertices.empty()) {
			const u8* b = (const u8*) &table.vertices[0];
			printf("  vertex 0 bytes");
			for (size_t i = 0; i < sizeof(MOBY::MobyVertex); i++) {
				printf(" %02x", b[i]);
			}
			printf("\n");
		}
	}
}

// mobyconvert --packets <core.bin> <game>
static int dump_packets(const char* path, Game game)
{
	std::vector<u8> input = read_file(fs::path(path));
	describe("class", input);
	MOBY::MobyClassData moby = MOBY::read_class(Buffer(input), game);
	dump_lod("high", moby.mesh.high_lod);
	dump_lod("low", moby.mesh.low_lod);
	return 0;
}

// The high and low LOD of an unskinned class, rebuilt from a glb (see --mesh above).
static void replace_mesh(MOBY::MobyClassData& moby, const char* path)
{
	verify(moby.animation.joint_count == 0, "--mesh only rebuilds classes without joints.");
	verify(moby.mesh.metal.empty() && moby.bangles.empty(), "--mesh only rebuilds classes without metal packets or bangles.");
	std::vector<u8> bytes = read_file(fs::path(path));
	GLTF::ModelFile gltf = GLTF::read_glb(bytes);
	GLTF::Node* node = GLTF::lookup_node(gltf, "moby");
	verify(node && node->mesh.has_value(), "%s has no node 'moby' with a mesh.", path);
	std::vector<Material> materials;
	for (const GLTF::Material& source : gltf.materials) {
		s32 texture = -1;
		verify(source.name.has_value() && sscanf(source.name->c_str(), "material_%d", &texture) == 1 && texture >= 0,
			"%s: materials must be named material_<texture index>.", path);
		Material& material = materials.emplace_back();
		material.name = *source.name;
		material.surface = MaterialSurface(texture);
	}
	GLTF::map_gltf_materials_to_wrench_materials(gltf, materials);
	const GLTF::Mesh& mesh = gltf.meshes.at(*node->mesh);
	auto [effectives, material_to_effective] = effective_materials(materials, MATERIAL_ATTRIB_SURFACE | MATERIAL_ATTRIB_WRAP_MODE);
	std::vector<GLTF::Mesh> parts = MOBY::split_packets(mesh, material_to_effective, false);
	std::vector<MOBY::MobyPacket> packets = MOBY::build_packets(parts, effectives, materials, moby.scale);
	verify(packets.size() < 256, "Too many packets (%zu).", packets.size());
	printf("mesh %s: %zu vertices, %zu primitives -> %zu packets per LOD\n", path, mesh.vertices.size(), mesh.primitives.size(), packets.size());

	// The builder leaves the matrix bytes of each vertex zero, so every vertex
	// would transfer joint 0's matrix to VU0 itself, and it fills the GS
	// texture registers and the index header its own way. The rebuilt packets
	// take the class's own conventions instead, which every retail R&C1 moby
	// shares: packet 0 transfers joint 0 to VU0 once and every vertex loads it
	// from there without storing (bytes 2 and 3), the index header's first
	// byte is the class's (0xfe), and the texture registers are the class's
	// first ones (TEX1's LOD values and GIF address 0x04) but for the texture.
	const std::vector<MOBY::MobyPacket>& old = moby.mesh.high_lod;
	verify(!old.empty() && !old[0].vif.textures.empty(), "The class has no textured packet to take conventions from.");
	const MOBY::MobyTexturePrimitive& registers = old[0].vif.textures[0];
	for (size_t p = 0; p < packets.size(); p++) {
		MOBY::MobyPacket& packet = packets[p];
		// The writer unpacks the index stream in whole quadwords and would drop
		// a tail (here the terminator); retail pads it with zeros.
		while (packet.vif.indices.size() % 4 != 0) {
			packet.vif.indices.push_back(0);
		}
		packet.vif.index_header_first_byte = old[0].vif.index_header_first_byte;
		packet.vertex_table.preloop_matrix_transfers = p == 0 ? old[0].vertex_table.preloop_matrix_transfers : std::vector<MOBY::MobyMatrixTransfer>{};
		u8 vu0 = old[0].vertex_table.preloop_matrix_transfers.empty() ? 0 : old[0].vertex_table.preloop_matrix_transfers[0].vu0_dest_addr;
		for (MOBY::MobyVertex& vertex : packet.vertex_table.vertices) {
			u8* b = (u8*) &vertex;
			// The 9-bit vertex ID names the cache slot later packets' duplicate
			// vertices copy the vertex from; the builder makes no duplicates and
			// leaves every ID 0. Retail gives a vertex nothing copies 0xff.
			vertex.v.i.low_halfword = 0xff;
			b[2] = vu0;
			b[3] = 0xf4;
		}
		for (MOBY::MobyTexturePrimitive& texture : packet.vif.textures) {
			s32 index = texture.d3_tex0_1.data_lo;
			texture.d1_tex1_1 = registers.d1_tex1_1;
			texture.d2_clamp_1 = registers.d2_clamp_1;
			texture.d3_tex0_1 = registers.d3_tex0_1;
			texture.d3_tex0_1.data_lo = index;
			texture.d4_miptbp1_1 = registers.d4_miptbp1_1;
		}
	}
	moby.mesh.high_lod = packets;
	moby.mesh.high_lod_count = (u8) packets.size();
	moby.mesh.low_lod = std::move(packets);
	moby.mesh.low_lod_count = moby.mesh.high_lod_count;
}

struct Vu0Slot
{
	s32 joint = -1; // The joint whose matrix is in the slot, -1 for a blend or unknown.
};

static u8 unused_vu0_address(const std::vector<MOBY::MobyPacket>& packets)
{
	s32 top = 0;
	for (const MOBY::MobyPacket& packet : packets) {
		const MOBY::VertexTable& table = packet.vertex_table;
		for (const MOBY::MobyMatrixTransfer& transfer : table.preloop_matrix_transfers) {
			top = std::max(top, (s32) transfer.vu0_dest_addr);
		}
		for (const MOBY::MobyVertex& vertex : table.vertices) {
			const u8* b = (const u8*) &vertex;
			for (s32 i = 2; i < 8; i++) {
				if (b[i] != 0xf4 && (i < 4 || i >= 6)) {
					top = std::max(top, (s32) b[i]);
				}
			}
		}
	}
	verify(top + 4 < 0xf4, "No free VU0 address.");
	return (u8) (top + 4);
}

static s32 rebind_packets(std::vector<MOBY::MobyPacket>& packets, s32 from, s32 to, s32 z_top, s32 z_bottom)
{
	u8 spare = unused_vu0_address(packets);
	s32 z_mid = (z_top + z_bottom) / 2;
	s32 changed = 0;
	Vu0Slot vu0[256];
	for (MOBY::MobyPacket& packet : packets) {
		MOBY::VertexTable& table = packet.vertex_table;
		for (const MOBY::MobyMatrixTransfer& transfer : table.preloop_matrix_transfers) {
			vu0[transfer.vu0_dest_addr].joint = transfer.spr_joint_index;
		}
		table.preloop_matrix_transfers.push_back({(u8) to, spare});
		vu0[spare].joint = to;
		size_t two = table.two_way_blend_vertex_count;
		size_t three = two + table.three_way_blend_vertex_count;
		for (size_t i = 0; i < table.vertices.size(); i++) {
			MOBY::MobyVertex& vertex = table.vertices[i];
			u8* b = (u8*) &vertex;
			s32 spr_joint = vertex.v.i.low_halfword >> 9;
			s32 z = vertex.v.z;
			if (i < two) {
				// loads b[2], b[3]; weights b[4], b[5]; transfer store b[6]; blend store b[7]
				if (b[6] != 0xf4) {
					vu0[b[6]].joint = spr_joint;
				}
				s32 j1 = vu0[b[2]].joint, j2 = vu0[b[3]].joint;
				if ((j1 == from && j2 == to) || (j1 == to && j2 == from)) {
					f32 t = std::clamp((z_top - z) / (f32) (z_top - z_bottom), 0.f, 1.f);
					s32 w_to = std::clamp((s32) roundf(t * 256.f), 1, 255);
					b[4] = (u8) (j1 == to ? w_to : 256 - w_to);
					b[5] = (u8) (256 - b[4]);
					changed++;
				} else if (z < z_mid && (j1 == from || j2 == from)) {
					b[j1 == from ? 2 : 3] = spare;
					changed++;
				}
				if (b[7] != 0xf4) {
					vu0[b[7]].joint = -1;
				}
			} else if (i < three) {
				// loads b[2], b[3] and the high bits of the low halfword times two; blend store b[7]
				if (z < z_mid) {
					if (vu0[b[2]].joint == from) { b[2] = spare; changed++; }
					if (vu0[b[3]].joint == from) { b[3] = spare; changed++; }
					if (vu0[spr_joint * 2].joint == from) {
						vertex.v.i.low_halfword = (vertex.v.i.low_halfword & 0x1ff) | ((spare / 2) << 9);
						changed++;
					}
				}
				if (b[7] != 0xf4) {
					vu0[b[7]].joint = -1;
				}
			} else {
				// transfer store b[3] (done first), load b[2]
				if (b[3] != 0xf4) {
					vu0[b[3]].joint = spr_joint;
				}
				if (z < z_mid && vu0[b[2]].joint == from) {
					b[2] = spare;
					changed++;
				}
			}
		}
	}
	return changed;
}

// Metal (multipass shine) vertices name their joints directly: joints in
// bytes 8..a, joint count in byte b, weights in bytes c..e.
static s32 rebind_metal_packets(std::vector<MOBY::MobyMetalPacket>& packets, s32 from, s32 to, s32 z_top, s32 z_bottom)
{
	s32 z_mid = (z_top + z_bottom) / 2;
	s32 changed = 0;
	for (MOBY::MobyMetalPacket& packet : packets) {
		for (MOBY::MetalVertex& vertex : packet.vertex_table.vertices) {
			u8* joints = &vertex.unknown_8;
			u8* weights = &vertex.unknown_c;
			u8 count = vertex.unknown_b;
			if (count == 2 && ((joints[0] == from && joints[1] == to) || (joints[0] == to && joints[1] == from))) {
				f32 t = std::clamp((z_top - vertex.z) / (f32) (z_top - z_bottom), 0.f, 1.f);
				s32 w_to = std::clamp((s32) roundf(t * 256.f), 1, 255);
				weights[0] = (u8) (joints[0] == to ? w_to : 256 - w_to);
				weights[1] = (u8) (256 - weights[0]);
				changed++;
			} else if (vertex.z < z_mid) {
				bool has_to = false;
				for (s32 i = 0; i < count && i < 3; i++) {
					has_to |= joints[i] == to;
				}
				for (s32 i = 0; i < count && i < 3 && !has_to; i++) {
					if (joints[i] == from) {
						joints[i] = (u8) to;
						changed++;
					}
				}
			}
		}
	}
	return changed;
}

int main(int argc, const char** argv)
{
	if (argc == 4 && strcmp(argv[1], "--skin") == 0) {
		return dump_skin(argv[2], parse_game(argv[3]));
	}
	if (argc == 4 && strcmp(argv[1], "--packets") == 0) {
		return dump_packets(argv[2], parse_game(argv[3]));
	}
	const char* mesh = nullptr;
	if (argc >= 7 && strcmp(argv[argc - 2], "--mesh") == 0) {
		mesh = argv[argc - 1];
		argc -= 2;
	}
	// --take-lod high|low <core.bin>: that LOD's packets from another class of the same game
	// (for isolating which packets a game rejects).
	const char* take_lod = nullptr;
	const char* take_from = nullptr;
	if (argc >= 8 && strcmp(argv[argc - 3], "--take-lod") == 0) {
		take_lod = argv[argc - 2];
		take_from = argv[argc - 1];
		argc -= 3;
	}
	const char** rebind = nullptr;
	if (argc >= 10 && strcmp(argv[argc - 5], "--rebind") == 0) {
		rebind = &argv[argc - 4];
		argc -= 5;
	}
	if (argc != 5 && argc != 6) {
		fprintf(stderr, "usage: %s <input core.bin> <input game> <output core.bin> <output game> [<reference core.bin>] [--rebind <from> <to> <z top> <z bottom>] [--mesh <glb>]\n",
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

	if (rebind) {
		s32 j_from = atoi(rebind[0]), j_to = atoi(rebind[1]), z_top = atoi(rebind[2]), z_bottom = atoi(rebind[3]);
		s32 high = rebind_packets(moby.mesh.high_lod, j_from, j_to, z_top, z_bottom);
		s32 low = rebind_packets(moby.mesh.low_lod, j_from, j_to, z_top, z_bottom);
		s32 metal = rebind_metal_packets(moby.mesh.metal, j_from, j_to, z_top, z_bottom);
		printf("rebind joint %d to %d below z %d..%d: %d high LOD, %d low LOD and %d metal vertices changed\n",
			j_from, j_to, z_top, z_bottom, high, low, metal);
	}

	if (mesh) {
		replace_mesh(moby, mesh);
	}
	if (take_lod) {
		std::vector<u8> other_bytes = read_file(fs::path(take_from));
		MOBY::MobyClassData other = MOBY::read_class(Buffer(other_bytes), to);
		if (strncmp(take_lod, "low:", 4) == 0) {
			// low:<first>:<count>: only those packets of the low LOD
			s32 first = 0, count = 0;
			verify(sscanf(take_lod, "low:%d:%d", &first, &count) == 2, "low:<first>:<count>");
			for (s32 i = first; i < first + count; i++) {
				moby.mesh.low_lod.at(i) = other.mesh.low_lod.at(i);
			}
		} else if (strcmp(take_lod, "high") == 0) {
			moby.mesh.high_lod = other.mesh.high_lod;
			moby.mesh.high_lod_count = other.mesh.high_lod_count;
		} else {
			moby.mesh.low_lod = other.mesh.low_lod;
			moby.mesh.low_lod_count = other.mesh.low_lod_count;
		}
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
