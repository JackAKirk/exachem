/*
 * ExaChem: Open Source Exascale Computational Chemistry Software.
 *
 * Copyright 2023 Pacific Northwest National Laboratory, Battelle Memorial Institute.
 *
 * See LICENSE.txt for details
 */

#pragma once

#include "common/fcidump.hpp"
#if defined(USE_MACIS)
#include "macis.hpp"
#endif

using namespace tamm;

template<typename T>
std::string generate_fcidump(SystemData sys_data, ExecutionContext& ec, const TiledIndexSpace& MSO,
                             Tensor<T>& lcao, Tensor<T>& d_f1, Tensor<T>& full_v2,
                             ExecutionHW ex_hw = ExecutionHW::CPU) {
  // int nactv = sys_data.options_map.fci_options.nactive;
  Scheduler sch{ec};

  std::cout.precision(15);
  // const auto rank = ec.pg().rank();

  auto [z1, z2] = MSO.labels<2>("all");

  // Transform d_f1 from Fock operator to the one-electron operator representation
  TiledIndexSpace AO = lcao.tiled_index_spaces()[0];
  auto [mu, nu]      = AO.labels<2>("all");

  Tensor<T> hcore{AO, AO};
  Tensor<T> hcore_mo{{MSO, MSO}, {1, 1}};
  Tensor<T>::allocate(&ec, hcore, hcore_mo);

  std::string out_fp = sys_data.output_file_prefix + "." + sys_data.options_map.ccsd_options.basis;
  std::string files_dir = out_fp + "_files/" + sys_data.options_map.scf_options.scf_type + "/fci";
  std::string files_prefix = /*out_fp;*/ files_dir + "/" + out_fp;
  if(!fs::exists(files_dir)) fs::create_directories(files_dir);

  std::string hcorefile = files_dir + "/../scf/" + out_fp + ".hcore";
  read_from_disk(hcore, hcorefile);

  Tensor<T> tmp{MSO, AO};
  // clang-format off
  sch.allocate(tmp)
    (tmp(z1,nu) = lcao(mu,z1) * hcore(mu,nu))
    (hcore_mo(z1,z2) = tmp(z1,nu) * lcao(nu,z2))
    .deallocate(tmp,hcore).execute();
  // clang-format on

  std::vector<int> symvec(sys_data.nbf_orig);
  if(sys_data.is_unrestricted) symvec.resize(2 * sys_data.nbf_orig);
  std::fill(symvec.begin(), symvec.end(), 1);

  // write fcidump file
  std::string fcid_file = files_prefix + ".fcidump";
  fcidump::write_fcidump_file(sys_data, hcore_mo, full_v2, symvec, fcid_file);

  free_tensors(hcore_mo);
  return files_prefix;
}

void fci_driver(std::string filename, OptionsMap options_map) {
  using T = double;

  ProcGroup        pg = ProcGroup::create_world_coll();
  ExecutionContext ec{pg, DistributionKind::nw, MemoryManagerKind::ga};
  auto             rank = ec.pg().rank();

  auto [sys_data, hf_energy, shells, shell_tile_map, C_AO, F_AO, C_beta_AO, F_beta_AO, AO_opt,
        AO_tis, scf_conv] = hartree_fock_driver<T>(ec, filename, options_map);

  CCSDOptions& ccsd_options = sys_data.options_map.ccsd_options;
  if(rank == 0) ccsd_options.print();

  if(rank == 0)
    cout << endl << "#occupied, #virtual = " << sys_data.nocc << ", " << sys_data.nvir << endl;

  auto [MO, total_orbitals] = setupMOIS(sys_data);

  std::string out_fp       = sys_data.output_file_prefix + "." + ccsd_options.basis;
  std::string files_dir    = out_fp + "_files/" + sys_data.options_map.scf_options.scf_type;
  std::string files_prefix = /*out_fp;*/ files_dir + "/" + out_fp;
  std::string f1file       = files_prefix + ".f1_mo";
  std::string v2file       = files_prefix + ".cholv2";
  std::string cholfile     = files_prefix + ".cholcount";

  ExecutionHW ex_hw = ec.exhw();

  bool ccsd_restart = ccsd_options.readt || (fs::exists(f1file) && fs::exists(v2file));

  // deallocates F_AO, C_AO
  auto [cholVpr, d_f1, lcao, chol_count, max_cvecs, CI] =
    cd_svd_driver<T>(sys_data, ec, MO, AO_opt, C_AO, F_AO, C_beta_AO, F_beta_AO, shells,
                     shell_tile_map, ccsd_restart, cholfile);

  TiledIndexSpace N = MO("all");

  if(ccsd_restart) {
    read_from_disk(d_f1, f1file);
    read_from_disk(cholVpr, v2file);
    ec.pg().barrier();
  }

  else if(ccsd_options.writet) {
    // fs::remove_all(files_dir);
    if(!fs::exists(files_dir)) fs::create_directories(files_dir);

    write_to_disk(d_f1, f1file);
    write_to_disk(cholVpr, v2file);

    if(rank == 0) {
      std::ofstream out(cholfile, std::ios::out);
      if(!out) cerr << "Error opening file " << cholfile << endl;
      out << chol_count << std::endl;
      out.close();
    }
  }

  ec.pg().barrier();

  auto [cindex]     = CI.labels<1>("all");
  auto [p, q, r, s] = MO.labels<4>("all");

  Tensor<T> full_v2{N, N, N, N};
  Tensor<T>::allocate(&ec, full_v2);

  // clang-format off
  Scheduler sch{ec};
  sch(full_v2(p, r, q, s)  = cholVpr(p, r, cindex) * cholVpr(q, s, cindex)).execute(ex_hw);
  // clang-format off

  free_tensors(cholVpr);

  files_prefix = generate_fcidump(sys_data, ec, MO, lcao, d_f1, full_v2, ex_hw);
  #if defined(USE_MACIS)
  if(options_map.task_options.fci)
    macis_driver(ec, sys_data, files_prefix);
  #endif
  
  free_tensors(lcao, d_f1, full_v2);

  ec.flush_and_sync();
  // delete ec;
}
